// SPDX-License-Identifier: GPL-2.0-only
#include "export/ptrace_util.h"

#include <cerrno>
#include <cstring>

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/util.h"

namespace dnuma {

Status read_proc_maps(pid_t pid, std::vector<ProcVma>* out) {
    std::string text;
    DNUMA_TRY(read_file("/proc/" + std::to_string(pid) + "/maps", &text));
    out->clear();
    for (const std::string& line : split(text, '\n')) {
        ProcVma v;
        char perms[8] = "";
        unsigned long long start = 0, end = 0, off = 0;
        int name_at = 0;
        if (sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &start, &end, perms, &off, &name_at) < 4) continue;
        v.start = start;
        v.end = end;
        v.offset = off;
        v.prot = (perms[0] == 'r' ? kVmRead : 0) | (perms[1] == 'w' ? kVmWrite : 0) | (perms[2] == 'x' ? kVmExec : 0);
        v.shared = perms[3] == 's';
        if (name_at > 0 && size_t(name_at) < line.size()) v.name = line.substr(size_t(name_at));
        out->push_back(std::move(v));
    }
    return Status::ok();
}

Status pt_getregs(pid_t pid, user_regs_struct* r) {
    if (ptrace(PTRACE_GETREGS, pid, 0, r) != 0) return Status::sys("PTRACE_GETREGS %d", pid);
    return Status::ok();
}

Status pt_setregs(pid_t pid, const user_regs_struct& r) {
    if (ptrace(PTRACE_SETREGS, pid, 0, &r) != 0) return Status::sys("PTRACE_SETREGS %d", pid);
    return Status::ok();
}

Status pt_get_xstate(pid_t pid, std::vector<uint8_t>* out) {
    // The regset size depends on the CPU's enabled features; grow until the
    // kernel returns less than we offered.
    for (size_t cap = 4096;; cap *= 2) {
        out->assign(cap, 0);
        iovec iov{out->data(), cap};
        if (ptrace(PTRACE_GETREGSET, pid, NT_X86_XSTATE, &iov) != 0) return Status::sys("PTRACE_GETREGSET(xstate) %d", pid);
        if (iov.iov_len < cap) {
            out->resize(iov.iov_len);
            return Status::ok();
        }
        if (cap > (64u << 20)) return Status::err(E2BIG, "unreasonable xstate size");
    }
}

Status pt_set_xstate(pid_t pid, const std::vector<uint8_t>& xs) {
    iovec iov{const_cast<uint8_t*>(xs.data()), xs.size()};
    if (ptrace(PTRACE_SETREGSET, pid, NT_X86_XSTATE, &iov) != 0) return Status::sys("PTRACE_SETREGSET(xstate) %d", pid);
    return Status::ok();
}

Status pt_wait_stop(pid_t pid, int* status) {
    for (;;) {
        pid_t r = waitpid(pid, status, __WALL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::sys("waitpid %d", pid);
        }
        if (WIFSTOPPED(*status)) return Status::ok();
        return Status::err(ESRCH, "process %d exited while being controlled", pid);
    }
}

Status pt_interrupt(pid_t pid, int* status) {
    if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) != 0) return Status::sys("PTRACE_INTERRUPT %d", pid);
    return pt_wait_stop(pid, status);
}

bool is_event_stop(int status) { return WIFSTOPPED(status) && (status >> 16) == PTRACE_EVENT_STOP; }
bool is_syscall_stop(int status) { return WIFSTOPPED(status) && stop_signal(status) == (SIGTRAP | 0x80); }

Status pt_inject(pid_t pid, uint64_t sys_insn, long nr, const uint64_t args[6], long* ret) {
    user_regs_struct saved, r;
    DNUMA_TRY(pt_getregs(pid, &saved));
    r = saved;
    r.rip = sys_insn;
    r.rax = uint64_t(nr);
    r.orig_rax = uint64_t(-1); // not "inside a system call": no restart fix-up on resume
    r.rdi = args[0];
    r.rsi = args[1];
    r.rdx = args[2];
    r.r10 = args[3];
    r.r8 = args[4];
    r.r9 = args[5];
    DNUMA_TRY(pt_setregs(pid, r));
    int st = 0;
    // Entry stop, then exit stop.
    for (int phase = 0; phase < 2; ++phase) {
        for (;;) {
            if (ptrace(PTRACE_SYSCALL, pid, 0, 0) != 0) return Status::sys("PTRACE_SYSCALL %d", pid);
            DNUMA_TRY(pt_wait_stop(pid, &st));
            if (is_syscall_stop(st)) break;
            // Any other stop here is a signal arriving meanwhile. It stays
            // pending for the tracee (we resume without delivering it now).
            if (is_event_stop(st)) continue;
            int sig = stop_signal(st);
            if (sig == SIGKILL) return Status::err(ESRCH, "process %d killed", pid);
            // Re-queue so it is not lost.
            if (sig != SIGTRAP) kill(pid, sig);
        }
    }
    DNUMA_TRY(pt_getregs(pid, &r));
    *ret = long(r.rax);
    return pt_setregs(pid, saved);
}

Status mem_read(int memfd, uint64_t addr, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
        ssize_t n = pread(memfd, p, len, off_t(addr));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return Status::err(n < 0 ? errno : EFAULT, "reading process memory at 0x%llx", (unsigned long long)addr);
        p += n;
        addr += uint64_t(n);
        len -= size_t(n);
    }
    return Status::ok();
}

Status mem_write(int memfd, uint64_t addr, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len) {
        ssize_t n = pwrite(memfd, p, len, off_t(addr));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return Status::err(n < 0 ? errno : EFAULT, "writing process memory at 0x%llx", (unsigned long long)addr);
        p += n;
        addr += uint64_t(n);
        len -= size_t(n);
    }
    return Status::ok();
}

std::vector<uint8_t> regs_to_bytes(const user_regs_struct& r) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&r);
    return {p, p + sizeof r};
}

bool regs_from_bytes(const std::vector<uint8_t>& b, user_regs_struct* r) {
    if (b.size() != sizeof *r) return false;
    memcpy(r, b.data(), sizeof *r);
    return true;
}

} // namespace dnuma
