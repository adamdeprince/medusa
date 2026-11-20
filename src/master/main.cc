// SPDX-License-Identifier: GPL-2.0-only
// dnuma-master entry point.
#include <csignal>
#include <cstdio>
#include <cstring>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"

using namespace dnuma;

static void usage() {
    fprintf(stderr,
            "usage: dnuma-master [options]\n"
            "  --key-file <path>     pre-shared cluster key (default /etc/dnuma/cluster.key)\n"
            "  --ctl-sock <path>     control socket for dnumactl (default /run/dnuma/master.sock)\n"
            "  --app-sock <path>     application socket for dnuma-run (default /run/dnuma/app.sock)\n"
            "  --agent-port <n>      default agent management port (default 7710)\n"
            "  --pool-size <size>    registered page pool on this machine (default 256M)\n"
            "  --capacity <size>     memory this machine offers to distributed processes\n"
            "                        (default: half of MemAvailable at start)\n"
            "  --lease-ms <n>        slave lease duration (default 5000)\n"
            "  --renew-ms <n>        slave lease renewal interval (default 1000)\n"
            "  --log-level <level>   error|warn|info|debug|trace (default info)\n"
            "  --log-file <path>     append log output to a file\n"
            "  --trace-pages         log every page-ownership transition\n"
            "  --auto-export         move CPU-bound processes to idle slave CPUs automatically\n"
            "  --export-uid-min <n>  only export processes of users with uid >= n (default 1000)\n"
            "  --export-workers <n>  threads serving exported processes (default: one per 6 CPUs)\n"
            "  --no-kernel-view      do not show slave CPUs and exported work in this machine's\n"
            "                        /proc, even if the kernel can (CONFIG_DNUMA_VIEW)\n"
            "  --view-proc-only      show them in /proc only: do not also list slave CPUs in\n"
            "                        /sys/devices/system/cpu/online (which makes programs that\n"
            "                        ask the C library count them as online processors)\n"
            "  --realtime            run the service threads in a real-time scheduling class\n"
            "                        (SCHED_FIFO 1) instead of nice -20; see docs/export.md\n");
}

int main(int argc, char** argv) {
    MasterConfig cfg;
    LogLevel level = LogLevel::Info;
    bool realtime = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "dnuma-master: %s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        uint64_t v = 0;
        if (a == "--key-file") cfg.key_file = next();
        else if (a == "--ctl-sock") cfg.ctl_sock = next();
        else if (a == "--app-sock") cfg.app_sock = next();
        else if (a == "--agent-port") cfg.agent_port = uint16_t(atoi(next().c_str()));
        else if (a == "--pool-size" && parse_size(next(), &v)) cfg.pool_bytes = v;
        else if (a == "--capacity" && parse_size(next(), &v)) cfg.capacity_bytes = v;
        else if (a == "--lease-ms") cfg.lease_ms = uint32_t(atoi(next().c_str()));
        else if (a == "--renew-ms") cfg.renew_ms = uint32_t(atoi(next().c_str()));
        else if (a == "--log-level" && log_parse_level(next(), &level)) {}
        else if (a == "--log-file") {
            if (!log_to_file(next())) {
                fprintf(stderr, "dnuma-master: cannot open log file\n");
                return 2;
            }
        } else if (a == "--trace-pages") log_set_page_trace(true);
        else if (a == "--auto-export") cfg.export_auto = true;
        else if (a == "--export-uid-min") cfg.export_uid_min = uint32_t(atoi(next().c_str()));
        else if (a == "--export-workers") cfg.export_workers = unsigned(atoi(next().c_str()));
        else if (a == "--realtime") realtime = true;
        else if (a == "--no-kernel-view") cfg.kernel_view = false;
        else if (a == "--view-proc-only") cfg.view_online = false;
        else {
            usage();
            return a == "--help" ? 0 : 2;
        }
    }
    log_init("master", level);
    set_service_realtime(realtime);
    raise_priority();
    tune_allocator();
    if (cfg.lease_ms < 2 * cfg.renew_ms) {
        fprintf(stderr, "dnuma-master: --lease-ms must be at least twice --renew-ms\n");
        return 2;
    }

    // Signals are delivered through the reactor like every other event.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD); // exported processes are our tracees
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    Fd sfd(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));

    Reactor reactor;
    reactor.set_slow_hook(5000000, [](const char* kind, int fd, uint64_t ns) {
        char link[64], target[256] = "";
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n > 0) target[n] = 0;
        LOG_WARN("event loop stalled %.1f ms in a %s (fd %d %s)", double(ns) / 1e6, kind, fd, target);
    });
    Master master(reactor, cfg);
    Status st = master.init();
    if (!st.is_ok()) {
        LOG_ERROR("startup failed: %s", st.str().c_str());
        return 1;
    }
    (void)reactor.add_fd(sfd.get(), EPOLLIN, [&](uint32_t) {
        signalfd_siginfo si;
        bool quit = false;
        while (read(sfd.get(), &si, sizeof si) == ssize_t(sizeof si)) {
            if (si.ssi_signo == SIGCHLD) master.on_sigchld();
            else quit = true;
        }
        if (!quit) return;
        LOG_INFO("signal received: shutting down");
        master.request_shutdown([&] { reactor.stop(); });
    });
    reactor.run();
    return 0;
}
