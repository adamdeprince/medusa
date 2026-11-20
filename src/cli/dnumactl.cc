// SPDX-License-Identifier: GPL-2.0-only
// dnumactl: command-line management tool. Talks to dnuma-master over its
// local Unix-domain control socket; all output is produced by the master
// from live state.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/util.h"
#include "transport/messages.h"
#include "transport/stream.h"

using namespace dnuma;

namespace {

// Sends one request and streams the reply. Returns the master's exit code,
// or 125 if the master could not be reached.
int run_request(const std::string& sock, const std::vector<std::string>& args, std::string* captured) {
    Fd fd;
    Status st = unix_connect(sock, &fd);
    if (!st.is_ok()) {
        fprintf(stderr, "dnumactl: cannot reach dnuma-master at %s: %s\n", sock.c_str(), st.str().c_str());
        return 125;
    }
    CtlRequestMsg rq;
    rq.args = args;
    st = send_msg_blocking(fd.get(), make_msg(MsgType::CtlRequest, rq));
    if (!st.is_ok()) {
        fprintf(stderr, "dnumactl: %s\n", st.str().c_str());
        return 125;
    }
    for (;;) {
        Msg m;
        st = recv_msg_blocking(fd.get(), &m);
        if (!st.is_ok()) {
            fprintf(stderr, "dnumactl: connection to master lost: %s\n", st.str().c_str());
            return 125;
        }
        if (m.type == MsgType::CtlOutput) {
            CtlOutputMsg o;
            if (!parse_msg(m, &o)) continue;
            if (captured) *captured += o.text;
            else {
                fputs(o.text.c_str(), stdout);
                fflush(stdout);
            }
        } else if (m.type == MsgType::CtlDone) {
            CtlDoneMsg d;
            return parse_msg(m, &d) ? d.code : 125;
        }
    }
}

bool has(const std::vector<std::string>& args, const char* flag) {
    for (const auto& a : args)
        if (a == flag) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::string sock = getenv("DNUMA_CTL_SOCK") ? getenv("DNUMA_CTL_SOCK") : "/run/dnuma/master.sock";
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sock" && i + 1 < argc) sock = argv[++i];
        else args.push_back(a);
    }
    if (args.empty() || args[0] == "--help" || args[0] == "help") args = {"help"};

    // A hard disconnect destroys processes. Interactively, show the impact
    // and require explicit confirmation before sending the real request.
    bool hard = args.size() >= 3 && args[0] == "slave" && args[1] == "disconnect" && has(args, "--hard");
    if (hard && !has(args, "--yes") && !has(args, "--dry-run")) {
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "dnumactl: refusing to hard-disconnect non-interactively without --yes\n"
                            "          (use --dry-run to preview the impact)\n");
            return 2;
        }
        std::vector<std::string> preview = args;
        preview.push_back("--dry-run");
        int rc = run_request(sock, preview, nullptr);
        if (rc != 0) return rc;
        printf("\nThis will SIGKILL the processes listed above and discard their memory.\n"
               "Type the slave name (%s) to proceed: ", args[2].c_str());
        fflush(stdout);
        std::string answer;
        if (!std::getline(std::cin, answer) || trim(answer) != args[2]) {
            printf("Aborted; nothing was changed.\n");
            return 1;
        }
        args.push_back("--yes");
    }
    return run_request(sock, args, nullptr);
}
