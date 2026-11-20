// SPDX-License-Identifier: GPL-2.0-only
// dnuma-run: launches a program as the home image of a new distributed
// process.
//
// The launcher itself becomes the program (exec), so the home image is an
// ordinary child of the user's shell: its PID, terminal, file descriptors,
// exit status and signals behave exactly as for any native process. What
// dnuma-run adds is the environment the runtime needs to register with the
// master, and a fixed address-space layout so that code and data addresses
// agree between the images of the process on different machines.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/personality.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::string sock = getenv("DNUMA_SOCK") ? getenv("DNUMA_SOCK") : "/run/dnuma/app.sock";
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sock" && i + 1 < argc) sock = argv[++i];
        else if (a == "--help" || a == "-h") {
            printf("usage: dnuma-run [--sock <master application socket>] [--] <program> [args...]\n\n"
                   "The program must be linked against libdnuma (see dnuma_add_executable in CMake):\n"
                   "statically and non-PIE, so that every image of the process has the same layout.\n"
                   "Placement of memory and threads is chosen by the program through the libdnuma API.\n");
            return 0;
        } else if (a == "--") {
            ++i;
            break;
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr, "dnuma-run: no program given (try --help)\n");
        return 2;
    }
    struct stat st{};
    if (stat(sock.c_str(), &st) != 0) {
        fprintf(stderr, "dnuma-run: master application socket %s: %s (is dnuma-master running?)\n", sock.c_str(),
                strerror(errno));
        return 125;
    }
    setenv("DNUMA_SOCK", sock.c_str(), 1);
    // Belt and braces for programs that are not fully static: no address
    // randomization, so shared mappings land identically where possible.
    personality(ADDR_NO_RANDOMIZE);
    execvp(argv[i], argv + i);
    fprintf(stderr, "dnuma-run: cannot execute %s: %s\n", argv[i], strerror(errno));
    return 126;
}
