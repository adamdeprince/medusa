/* SPDX-License-Identifier: GPL-2.0-only */
// ptrace and /proc helpers shared by the master (which controls the original
// process) and the agent (which controls its body).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/user.h>

#include "common/status.h"
#include "export/xmsg.h"

namespace dnuma {

struct ProcVma {
    uint64_t start = 0, end = 0;
    uint32_t prot = 0;
    bool shared = false;
    uint64_t offset = 0;
    std::string name;
};

Status read_proc_maps(pid_t pid, std::vector<ProcVma>* out);

Status pt_getregs(pid_t pid, user_regs_struct* r);
Status pt_setregs(pid_t pid, const user_regs_struct& r);
// The complete extended register state (x87, SSE, AVX, ...).
Status pt_get_xstate(pid_t pid, std::vector<uint8_t>* out);
Status pt_set_xstate(pid_t pid, const std::vector<uint8_t>& xs);

// Blocking wait for the tracee's next stop. *status receives the raw wait
// status. Fails if the tracee exited instead of stopping.
Status pt_wait_stop(pid_t pid, int* status);
// Stops a running tracee (PTRACE_INTERRUPT) and waits for it.
Status pt_interrupt(pid_t pid, int* status);

// Makes a stopped tracee execute one system call and stop again. Only for
// calls that cannot block. `sys_insn` is the address of a `syscall`
// instruction in the tracee. Registers are restored afterwards.
Status pt_inject(pid_t pid, uint64_t sys_insn, long nr, const uint64_t args[6], long* ret);

// Memory of another process through /proc/pid/mem (works on read-only
// private mappings too: the write is a forced copy-on-write).
Status mem_read(int memfd, uint64_t addr, void* buf, size_t len);
Status mem_write(int memfd, uint64_t addr, const void* buf, size_t len);

// True if the wait status is a ptrace stop of the given kind.
bool is_event_stop(int status);   // PTRACE_EVENT_STOP (interrupt or group stop)
bool is_syscall_stop(int status); // syscall stop under PTRACE_O_TRACESYSGOOD
inline int stop_signal(int status) { return (status >> 8) & 0xff; }

std::vector<uint8_t> regs_to_bytes(const user_regs_struct& r);
bool regs_from_bytes(const std::vector<uint8_t>& b, user_regs_struct* r);

} // namespace dnuma
