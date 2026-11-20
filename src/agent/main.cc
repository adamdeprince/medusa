// SPDX-License-Identifier: GPL-2.0-only
// dnuma-agent entry point.
#include <csignal>
#include <cstdio>

#include <infiniband/verbs.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include "agent/agent.h"
#include "common/log.h"
#include "common/util.h"

using namespace dnuma;

static void usage() {
    fprintf(stderr,
            "usage: dnuma-agent [options]\n"
            "  --key-file <path>     pre-shared cluster key (default /etc/dnuma/cluster.key)\n"
            "  --listen <addr>       management listen address (default: all)\n"
            "  --port <n>            management port (default 7710)\n"
            "  --rdma-ip <addr>      address of the RDMA port to use (default: first ACTIVE\n"
            "                        InfiniBand port that has an IPv4 address)\n"
            "  --rdma-port <n>       RDMA listen port (default 7711)\n"
            "  --state-dir <path>    where the agent's stable identity is kept (default /var/lib/dnuma)\n"
            "  --nodes <list>        offer only these NUMA nodes (kernel numbers, e.g. 3 or 2-3);\n"
            "                        the agent and everything it hosts then use only those\n"
            "                        nodes' CPUs and memory (default: the whole machine)\n"
            "  --body-stub <path>    the dnuma-body executable (default: next to dnuma-agent)\n"
            "  --body-workers <n>    threads hosting exported processes (default: one per 6 CPUs)\n"
            "  --realtime            run the service threads in a real-time scheduling class\n"
            "                        (SCHED_FIFO 1) instead of nice -20; see docs/export.md\n"
            "  --pool-size <size>    registered page pool (default 256M)\n"
            "  --capacity <size>     memory offered to distributed processes\n"
            "                        (default: half of MemAvailable at start)\n"
            "  --log-level <level>   error|warn|info|debug|trace (default info)\n"
            "  --log-file <path>     append log output to a file\n"
            "  --trace-pages         log every local page transition\n");
}

int main(int argc, char** argv) {
    AgentConfig cfg;
    LogLevel level = LogLevel::Info;
    bool realtime = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "dnuma-agent: %s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        uint64_t v = 0;
        if (a == "--key-file") cfg.key_file = next();
        else if (a == "--listen") cfg.listen_addr = next();
        else if (a == "--port") cfg.port = uint16_t(atoi(next().c_str()));
        else if (a == "--rdma-ip") cfg.rdma_ip = next();
        else if (a == "--rdma-port") cfg.rdma_port = uint16_t(atoi(next().c_str()));
        else if (a == "--state-dir") cfg.state_dir = next();
        else if (a == "--body-stub") cfg.body_stub = next();
        else if (a == "--nodes") {
            std::string list = next();
            if (list.find_first_not_of("0123456789,-") == std::string::npos) cfg.nodes = parse_cpulist(list);
            if (cfg.nodes.empty()) {
                fprintf(stderr, "dnuma-agent: --nodes needs a list of NUMA node numbers, got '%s'\n", list.c_str());
                return 2;
            }
        }
        else if (a == "--body-workers") cfg.body_workers = unsigned(atoi(next().c_str()));
        else if (a == "--realtime") realtime = true;
        else if (a == "--pool-size" && parse_size(next(), &v)) cfg.pool_bytes = v;
        else if (a == "--capacity" && parse_size(next(), &v)) cfg.capacity_bytes = v;
        else if (a == "--log-level" && log_parse_level(next(), &level)) {}
        else if (a == "--log-file") {
            if (!log_to_file(next())) {
                fprintf(stderr, "dnuma-agent: cannot open log file\n");
                return 2;
            }
        } else if (a == "--trace-pages") log_set_page_trace(true);
        else {
            usage();
            return a == "--help" ? 0 : 2;
        }
    }
    log_init("agent", level);
    set_service_realtime(realtime);
    raise_priority();
    tune_allocator();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD); // bodies of exported processes are traced children
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
    Agent agent(reactor, cfg);
    Status st = agent.init();
    if (!st.is_ok()) {
        LOG_ERROR("startup failed: %s", st.str().c_str());
        return 1;
    }
    (void)reactor.add_fd(sfd.get(), EPOLLIN, [&](uint32_t) {
        signalfd_siginfo si;
        bool quit = false;
        while (read(sfd.get(), &si, sizeof si) == ssize_t(sizeof si)) {
            if (si.ssi_signo == SIGCHLD) agent.on_sigchld();
            else quit = true;
        }
        if (!quit) return;
        LOG_INFO("signal received: shutting down");
        agent.shutdown();
        reactor.stop();
    });
    reactor.run();
    return 0;
}
