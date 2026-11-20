// SPDX-License-Identifier: GPL-2.0-only
#include "export/sysdesc.h"

#include <unordered_map>

#include <linux/futex.h>
#include <sys/syscall.h>

namespace dnuma {

namespace {

using K = ArgKind;
constexpr ArgDesc V{};
constexpr ArgDesc STR{K::Str, 0, 0};
constexpr ArgDesc In(uint8_t len) { return {K::In, len, 0}; }
constexpr ArgDesc Out(uint8_t len) { return {K::Out, len, 0}; }
constexpr ArgDesc OutAll(uint8_t len) { return {K::OutAll, len, 0}; }
constexpr ArgDesc SIn(uint32_t size) { return {K::SIn, 0, size}; }
constexpr ArgDesc SOut(uint32_t size) { return {K::SOut, 0, size}; }
constexpr ArgDesc SIO(uint32_t size) { return {K::SInOut, 0, size}; }
constexpr ArgDesc IovIn(uint8_t cnt) { return {K::IovIn, cnt, 0}; }
constexpr ArgDesc IovOut(uint8_t cnt) { return {K::IovOut, cnt, 0}; }
constexpr ArgDesc Poll(uint8_t cnt) { return {K::PollFds, cnt, 0}; }

// Kernel ABI structure sizes (x86-64).
constexpr uint32_t kStat = 144, kTimespec = 16, kTimeval = 16, kRusage = 144, kSigaction = 32, kSigset = 8,
                   kStackT = 24, kRlimit = 16, kUtsname = 390, kSysinfo = 112, kStatfs = 120, kStatx = 256,
                   kTermios = 36, kWinsize = 8, kFlock = 32, kTms = 32, kTimezone = 8;

#define F SysClass::Forward
#define M SysClass::Memory
#define L SysClass::Local
#define H SysClass::Home
#define X SysClass::Exit

const SysDesc kTable[] = {
    {SYS_read, "read", F, {V, Out(2), V}},
    {SYS_write, "write", F, {V, In(2), V}},
    {SYS_open, "open", F, {STR, V, V}},
    {SYS_close, "close", F, {V}},
    {SYS_stat, "stat", F, {STR, SOut(kStat)}},
    {SYS_fstat, "fstat", F, {V, SOut(kStat)}},
    {SYS_lstat, "lstat", F, {STR, SOut(kStat)}},
    {SYS_poll, "poll", F, {Poll(1), V, V}},
    {SYS_lseek, "lseek", F, {V, V, V}},
    {SYS_mmap, "mmap", M, {V, V, V, V, V, V}},
    {SYS_mprotect, "mprotect", M, {V, V, V}},
    {SYS_munmap, "munmap", M, {V, V}},
    {SYS_brk, "brk", M, {V}},
    {SYS_rt_sigaction, "rt_sigaction", F, {V, SIn(kSigaction), SOut(kSigaction), V}},
    {SYS_rt_sigprocmask, "rt_sigprocmask", F, {V, SIn(kSigset), SOut(kSigset), V}},
    {SYS_pread64, "pread64", F, {V, Out(2), V, V}},
    {SYS_pwrite64, "pwrite64", F, {V, In(2), V, V}},
    {SYS_readv, "readv", F, {V, IovOut(2), V}},
    {SYS_writev, "writev", F, {V, IovIn(2), V}},
    {SYS_access, "access", F, {STR, V}},
    {SYS_pipe, "pipe", F, {SOut(8)}},
    {SYS_sched_yield, "sched_yield", L, {}},
    {SYS_dup, "dup", F, {V}},
    {SYS_dup2, "dup2", F, {V, V}},
    {SYS_nanosleep, "nanosleep", F, {SIn(kTimespec), SOut(kTimespec)}},
    {SYS_getpid, "getpid", F, {}},
    {SYS_sendfile, "sendfile", F, {V, V, SIO(8), V}},
    {SYS_exit, "exit", X, {V}},
    {SYS_wait4, "wait4", F, {V, SOut(4), V, SOut(kRusage)}},
    {SYS_kill, "kill", F, {V, V}},
    {SYS_uname, "uname", F, {SOut(kUtsname)}},
    {SYS_flock, "flock", F, {V, V}},
    {SYS_fsync, "fsync", F, {V}},
    {SYS_fdatasync, "fdatasync", F, {V}},
    {SYS_truncate, "truncate", F, {STR, V}},
    {SYS_ftruncate, "ftruncate", F, {V, V}},
    {SYS_getdents64, "getdents64", F, {V, Out(2), V}},
    {SYS_getcwd, "getcwd", F, {Out(1), V}},
    {SYS_chdir, "chdir", F, {STR}},
    {SYS_fchdir, "fchdir", F, {V}},
    {SYS_rename, "rename", F, {STR, STR}},
    {SYS_mkdir, "mkdir", F, {STR, V}},
    {SYS_rmdir, "rmdir", F, {STR}},
    {SYS_creat, "creat", F, {STR, V}},
    {SYS_link, "link", F, {STR, STR}},
    {SYS_unlink, "unlink", F, {STR}},
    {SYS_symlink, "symlink", F, {STR, STR}},
    {SYS_readlink, "readlink", F, {STR, Out(2), V}},
    {SYS_chmod, "chmod", F, {STR, V}},
    {SYS_fchmod, "fchmod", F, {V, V}},
    {SYS_chown, "chown", F, {STR, V, V}},
    {SYS_fchown, "fchown", F, {V, V, V}},
    {SYS_lchown, "lchown", F, {STR, V, V}},
    {SYS_umask, "umask", F, {V}},
    {SYS_gettimeofday, "gettimeofday", F, {SOut(kTimeval), SOut(kTimezone)}},
    {SYS_getrlimit, "getrlimit", F, {V, SOut(kRlimit)}},
    {SYS_getrusage, "getrusage", F, {V, SOut(kRusage)}},
    {SYS_sysinfo, "sysinfo", F, {SOut(kSysinfo)}},
    {SYS_times, "times", F, {SOut(kTms)}},
    {SYS_getuid, "getuid", F, {}},
    {SYS_getgid, "getgid", F, {}},
    {SYS_geteuid, "geteuid", F, {}},
    {SYS_getegid, "getegid", F, {}},
    {SYS_getppid, "getppid", F, {}},
    {SYS_getpgrp, "getpgrp", F, {}},
    {SYS_getpgid, "getpgid", F, {V}},
    {SYS_getsid, "getsid", F, {V}},
    {SYS_setpgid, "setpgid", F, {V, V}},
    {SYS_sigaltstack, "sigaltstack", F, {SIn(kStackT), SOut(kStackT)}},
    {SYS_statfs, "statfs", F, {STR, SOut(kStatfs)}},
    {SYS_fstatfs, "fstatfs", F, {V, SOut(kStatfs)}},
    {SYS_getpriority, "getpriority", F, {V, V}},
    {SYS_setpriority, "setpriority", F, {V, V, V}},
    {SYS_arch_prctl, "arch_prctl", L, {V, V}},
    {SYS_setrlimit, "setrlimit", F, {V, SIn(kRlimit)}},
    {SYS_sync, "sync", F, {}},
    {SYS_gettid, "gettid", F, {}},
    {SYS_time, "time", F, {SOut(8)}},
    {SYS_sched_getaffinity, "sched_getaffinity", F, {V, V, Out(1)}},
    {SYS_set_tid_address, "set_tid_address", F, {V}},
    {SYS_fadvise64, "fadvise64", F, {V, V, V, V}},
    {SYS_clock_gettime, "clock_gettime", F, {V, SOut(kTimespec)}},
    {SYS_clock_getres, "clock_getres", F, {V, SOut(kTimespec)}},
    {SYS_clock_nanosleep, "clock_nanosleep", F, {V, V, SIn(kTimespec), SOut(kTimespec)}},
    {SYS_exit_group, "exit_group", X, {V}},
    {SYS_tgkill, "tgkill", F, {V, V, V}},
    {SYS_openat, "openat", F, {V, STR, V, V}},
    {SYS_mkdirat, "mkdirat", F, {V, STR, V}},
    {SYS_fchownat, "fchownat", F, {V, STR, V, V, V}},
    {SYS_newfstatat, "newfstatat", F, {V, STR, SOut(kStat), V}},
    {SYS_unlinkat, "unlinkat", F, {V, STR, V}},
    {SYS_renameat, "renameat", F, {V, STR, V, STR}},
    {SYS_linkat, "linkat", F, {V, STR, V, STR, V}},
    {SYS_symlinkat, "symlinkat", F, {STR, V, STR}},
    {SYS_readlinkat, "readlinkat", F, {V, STR, Out(3), V}},
    {SYS_fchmodat, "fchmodat", F, {V, STR, V}},
    {SYS_faccessat, "faccessat", F, {V, STR, V}},
    {SYS_ppoll, "ppoll", F, {Poll(1), V, SIn(kTimespec), SIn(kSigset), V}},
    {SYS_set_robust_list, "set_robust_list", F, {V, V}},
    {SYS_utimensat, "utimensat", F, {V, STR, SIn(2 * kTimespec), V}},
    {SYS_fallocate, "fallocate", F, {V, V, V, V}},
    {SYS_dup3, "dup3", F, {V, V, V}},
    {SYS_pipe2, "pipe2", F, {SOut(8), V}},
    {SYS_prlimit64, "prlimit64", F, {V, V, SIn(kRlimit), SOut(kRlimit)}},
    {SYS_getcpu, "getcpu", L, {V, V, V}},
    {SYS_renameat2, "renameat2", F, {V, STR, V, STR, V}},
    {SYS_getrandom, "getrandom", F, {Out(1), V, V}},
    {SYS_copy_file_range, "copy_file_range", F, {V, SIO(8), V, SIO(8), V, V}},
    {SYS_statx, "statx", F, {V, STR, V, V, SOut(kStatx)}},
    {SYS_rseq, "rseq", F, {V, V, V, V}},
    {SYS_faccessat2, "faccessat2", F, {V, STR, V, V}},
    {SYS_close_range, "close_range", F, {V, V, V}},
};

// Request-dependent calls.
const SysDesc kIoctlTermios{SYS_ioctl, "ioctl(TCGETS)", F, {V, V, SOut(kTermios)}};
const SysDesc kIoctlWinsize{SYS_ioctl, "ioctl(TIOCGWINSZ)", F, {V, V, SOut(kWinsize)}};
const SysDesc kIoctlInt{SYS_ioctl, "ioctl(int*)", F, {V, V, SOut(4)}};
const SysDesc kIoctlNoArg{SYS_ioctl, "ioctl(noarg)", F, {V, V, V}};
const SysDesc kFcntlVal{SYS_fcntl, "fcntl", F, {V, V, V}};
const SysDesc kFcntlLock{SYS_fcntl, "fcntl(lock)", F, {V, V, SIO(kFlock)}};
const SysDesc kFutexWake{SYS_futex, "futex(wake)", L, {V, V, V}};
const SysDesc kPrctlSetName{SYS_prctl, "prctl(PR_SET_NAME)", F, {V, SIn(16)}};
const SysDesc kPrctlGetName{SYS_prctl, "prctl(PR_GET_NAME)", F, {V, SOut(16)}};
const SysDesc kMadviseHint{SYS_madvise, "madvise(hint)", L, {V, V, V}};
const SysDesc kMadviseDrop{SYS_madvise, "madvise(drop)", M, {V, V, V}};
const SysDesc kHome{-1, "unsupported", H, {}};

#undef F
#undef M
#undef L
#undef H
#undef X

const std::unordered_map<int64_t, const SysDesc*>& table() {
    static const auto* t = [] {
        auto* m = new std::unordered_map<int64_t, const SysDesc*>;
        for (const SysDesc& d : kTable) (*m)[d.nr] = &d;
        return m;
    }();
    return *t;
}

} // namespace

const SysDesc& describe_syscall(int64_t nr, const uint64_t args[6]) {
    switch (nr) {
    case SYS_ioctl:
        switch (args[1]) {
        case 0x5401: return kIoctlTermios;                 // TCGETS
        case 0x5413: return kIoctlWinsize;                 // TIOCGWINSZ
        case 0x541B: case 0x540F: return kIoctlInt;        // FIONREAD, TIOCGPGRP
        case 0x5450: case 0x5451: return kIoctlNoArg;      // FIONCLEX, FIOCLEX
        default: return kHome;
        }
    case SYS_fcntl:
        switch (args[1]) {
        case 0: case 1: case 2: case 3: case 4: case 1030: // DUPFD, GETFD, SETFD, GETFL, SETFL, DUPFD_CLOEXEC
        case 1024: case 1025:                              // F_SETLEASE, F_GETLEASE
            return kFcntlVal;
        case 5: case 6: case 7: case 36: case 37: case 38: // GETLK, SETLK, SETLKW, OFD variants
            return kFcntlLock;
        default: return kHome;
        }
    case SYS_futex:
        // A process with one thread has no private waiters to wake. Anything
        // else (waiting, shared futexes) needs the real kernel queues.
        if ((args[1] & 0x7f) == FUTEX_WAKE && (args[1] & FUTEX_PRIVATE_FLAG)) return kFutexWake;
        return kHome;
    case SYS_prctl:
        if (args[0] == 15) return kPrctlSetName;
        if (args[0] == 16) return kPrctlGetName;
        return kHome;
    case SYS_madvise:
        // DONTNEED / FREE / REMOVE discard contents; the rest are hints.
        if (args[2] == 4 || args[2] == 8 || args[2] == 9) return kMadviseDrop;
        return kMadviseHint;
    default: {
        auto it = table().find(nr);
        return it == table().end() ? kHome : *it->second;
    }
    }
}

} // namespace dnuma
