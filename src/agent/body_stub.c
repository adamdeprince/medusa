// SPDX-License-Identifier: GPL-2.0-only
/*
 * dnuma-body: the empty shell an exported process's user-mode code runs in.
 *
 * Freestanding, static, linked at an out-of-the-way address. It creates a
 * userfaultfd for its own address space, hands it to the agent, drops to the
 * owner's credentials and stops. From then on the agent drives it entirely
 * through ptrace: it unmaps everything but this code page, mirrors the
 * original process's mappings, loads its registers and lets it run under
 * PTRACE_SYSEMU, so no system call the process makes ever executes on this
 * machine.
 */
#include <linux/ioctl.h>
#include <linux/userfaultfd.h>

#define SYS_read 0
#define SYS_close 3
#define SYS_ioctl 16
#define SYS_getpid 39
#define SYS_sendmsg 46
#define SYS_exit 60
#define SYS_kill 62
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_setgroups 116
#define SYS_prctl 157
#define SYS_userfaultfd 323

#define PR_SET_PDEATHSIG 1
#define SIGKILL_ 9
#define O_CLOEXEC_ 02000000
#define O_NONBLOCK_ 04000
#define SOL_SOCKET_ 1
#define SCM_RIGHTS_ 1

static inline long sc(long n, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

/* The one instruction sequence the agent needs from us afterwards. */
__asm__(".text\n"
        ".globl dnuma_gadget\n"
        "dnuma_gadget:\n"
        "    syscall\n"
        "    int3\n");
extern char dnuma_gadget[] __attribute__((visibility("hidden")));

static unsigned long parse(const char* s) {
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

struct iovec_ { void* base; unsigned long len; };
struct msghdr_ {
    void* name; unsigned int namelen; struct iovec_* iov; unsigned long iovlen;
    void* control; unsigned long controllen; int flags;
};
struct cmsg_ { unsigned long len; int level; int type; int fd; int pad; };

void stub_main(long* sp) {
    long argc = sp[0];
    char** argv = (char**)(sp + 1);
    unsigned int uid = argc > 1 ? (unsigned int)parse(argv[1]) : 65534;
    unsigned int gid = argc > 2 ? (unsigned int)parse(argv[2]) : 65534;

    sc(SYS_prctl, PR_SET_PDEATHSIG, SIGKILL_, 0, 0, 0, 0); /* never outlive the agent */

    /* This process takes no faults in kernel mode (it makes no system calls
     * of its own once it runs), so a user-mode-only descriptor is enough. */
    long uffd = sc(SYS_userfaultfd, O_CLOEXEC_ | O_NONBLOCK_, 0, 0, 0, 0, 0);
    if (uffd < 0) uffd = sc(SYS_userfaultfd, O_CLOEXEC_ | O_NONBLOCK_ | UFFD_USER_MODE_ONLY, 0, 0, 0, 0, 0);
    if (uffd < 0) sc(SYS_exit, 71, 0, 0, 0, 0, 0);
    struct uffdio_api api;
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
    api.ioctls = 0;
    if (sc(SYS_ioctl, uffd, UFFDIO_API, (long)&api, 0, 0, 0) != 0) sc(SYS_exit, 72, 0, 0, 0, 0, 0);

    char byte = 'u';
    struct iovec_ iov = {&byte, 1};
    struct cmsg_ cm = {20 /* CMSG_LEN(sizeof(int)) */, SOL_SOCKET_, SCM_RIGHTS_, (int)uffd, 0};
    struct msghdr_ mh = {0, 0, &iov, 1, &cm, 24 /* CMSG_SPACE(sizeof(int)) */, 0};
    if (sc(SYS_sendmsg, 3, (long)&mh, 0, 0, 0, 0) != 1) sc(SYS_exit, 73, 0, 0, 0, 0, 0);

    /* Run as the owner of the process from here on. */
    if (sc(SYS_setgroups, 1, (long)&gid, 0, 0, 0, 0) != 0 || sc(SYS_setgid, gid, 0, 0, 0, 0, 0) != 0 ||
        sc(SYS_setuid, uid, 0, 0, 0, 0, 0) != 0)
        sc(SYS_exit, 74, 0, 0, 0, 0, 0);
    sc(SYS_close, 3, 0, 0, 0, 0, 0);
    sc(SYS_close, uffd, 0, 0, 0, 0, 0);

    /* Report where the gadget is and stop for the agent. */
    __asm__ volatile("int3" : : "a"((unsigned long)dnuma_gadget));
    for (;;) sc(SYS_exit, 75, 0, 0, 0, 0, 0);
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call stub_main\n"
        "    ud2\n");
