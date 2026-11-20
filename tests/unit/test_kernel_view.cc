// SPDX-License-Identifier: GPL-2.0-only
// The master's kernel-view client: the text it produces, and - when the
// running kernel has the view and the test runs as root - the kernel
// interface itself (what /proc and sysfs show after each command).
#include <csignal>
#include <cstring>

#include <sys/wait.h>
#include <unistd.h>

#include "common/util.h"
#include "master/kernel_view.h"
#include "utest.h"

using namespace dnuma;

static const char kSample[] =
    "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Test CPU @ 2.40GHz\nphysical id\t: 0\ncore id\t\t: 0\n"
    "flags\t\t: fpu vme\npower management:\n\n"
    "processor\t: 7\nvendor_id\t: GenuineIntel\nmodel name\t: Test CPU @ 2.40GHz\nphysical id\t: 3\ncore id\t\t: 1\n"
    "flags\t\t: fpu vme\npower management:\n\n";

static size_t count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + 1)) ++n;
    return n;
}
static std::string slurp(const std::string& path) {
    std::string s;
    (void)read_file(path, &s);
    return s;
}
// Field `n` (as numbered in proc(5)) of a /proc/<pid>/stat line.
static std::string stat_field(const std::string& stat, size_t n) {
    size_t paren = stat.rfind(')');
    if (paren == std::string::npos) return "";
    std::vector<std::string> f = split(trim(stat.substr(paren + 2)), ' ');
    return n >= 3 && n - 3 < f.size() ? f[n - 3] : "";
}

TEST(cpuinfo_split) {
    auto blocks = split_cpuinfo(kSample);
    CHECK_EQ(blocks.size(), 2u);
    CHECK_EQ(blocks[0].first, 0);
    CHECK_EQ(blocks[1].first, 7);
    CHECK(blocks[1].second.find("physical id\t: 3") != std::string::npos);
    CHECK(blocks[0].second.size() > 2 && blocks[0].second.compare(blocks[0].second.size() - 2, 2, "\n\n") == 0);
    CHECK_EQ(max_physical_id(kSample), 3);
    CHECK_EQ(max_physical_id("processor\t: 0\n\n"), -1);
    CHECK(split_cpuinfo("").empty());
}

TEST(cpuinfo_rewrite) {
    auto blocks = split_cpuinfo(kSample);
    std::string b = remote_cpuinfo_block(blocks[1].second, 123, 4, "other-host");
    CHECK(b.find("processor\t: 123\n") == 0);
    CHECK(b.find("physical id\t: 7\n") != std::string::npos);
    CHECK(b.find("model name\t: Test CPU @ 2.40GHz\n") != std::string::npos);
    CHECK(b.find("power management:\n") != std::string::npos);
    CHECK(b.find("dnuma host\t: other-host\n\n") == b.size() - strlen("dnuma host\t: other-host\n\n"));
    // What it produces is a block the same parser understands.
    auto again = split_cpuinfo(b);
    CHECK(again.size() == 1 && again[0].first == 123);
}

TEST(live_kernel_interface) {
    KernelView v;
    std::string why;
    if (!v.open(&why)) {
        printf("    (not run: %s)\n", why.c_str());
        return;
    }
    const int base = v.nr_cpu_ids();
    const size_t local = count(slurp("/proc/cpuinfo"), "\nprocessor\t:") + 1;
    const std::string online0 = trim(slurp("/sys/devices/system/cpu/online"));
    auto blocks = split_cpuinfo(kSample);

    for (int i = 0; i < 3; ++i) v.set_cpu(base + i, remote_cpuinfo_block(blocks[1].second, base + i, 10, "testhost"));
    uint64_t st[kViewStatFields] = {11, 0, 22, 33, 0, 0, 0, 0, 0, 0};
    v.cpu_stat(base + 1, st);
    v.flush();
    std::string ci = slurp("/proc/cpuinfo");
    CHECK_EQ(count(ci, "dnuma host\t: testhost\n"), 3u);
    CHECK_EQ(count(ci, "\nprocessor\t:") + 1, local + 3);
    CHECK(ci.find(format("processor\t: %d\n", base + 2)) != std::string::npos);
    CHECK(slurp("/proc/stat").find(format("\ncpu%d 11 0 22 33 0 0 0 0 0 0\n", base + 1)) != std::string::npos);

    CHECK(trim(slurp("/sys/devices/system/cpu/online")) == online0);
    v.set_online(true);
    v.flush();
    CHECK(trim(slurp("/sys/devices/system/cpu/online")) == online0 + format(",%d-%d", base, base + 2));
    CHECK_EQ(sysconf(_SC_NPROCESSORS_ONLN), long(local + 3));

    pid_t child = fork();
    if (child == 0) {
        for (;;) pause();
    }
    usleep(50000);
    std::string path = format("/proc/%d/stat", int(child));
    uint64_t own = strtoull(stat_field(slurp(path), 14).c_str(), nullptr, 10);
    v.set_task(child, base + 2, true, 500, 20);
    v.flush();
    std::string s = slurp(path);
    CHECK(stat_field(s, 3) == std::string("R"));
    CHECK(stat_field(s, 14) == std::to_string(own + 500));
    CHECK(stat_field(s, 15) == std::string("20"));
    CHECK(stat_field(s, 39) == std::to_string(base + 2));
    v.set_task(child, -1, false, 700, 20);
    v.flush();
    s = slurp(path);
    CHECK(stat_field(s, 3) == std::string("S"));
    CHECK(stat_field(s, 14) == std::to_string(own + 700));
    CHECK(atoi(stat_field(s, 39).c_str()) < base);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    v.set_task(child, base, true, 1, 1); // gone: must not disturb the commands after it
    v.del_cpu(base);
    v.flush();
    CHECK_EQ(count(slurp("/proc/cpuinfo"), "dnuma host\t: testhost\n"), 2u);

    // More than fits in one write: the client has to split the batch.
    std::string big = blocks[0].second;
    big.insert(big.find("power management"), "flags2\t\t: " + std::string(6000, 'x') + "\n");
    for (int i = 0; i < 300; ++i) v.set_cpu(base + 100 + i, remote_cpuinfo_block(big, base + 100 + i, 0, "bulk"));
    v.flush();
    CHECK_EQ(count(slurp("/proc/cpuinfo"), "dnuma host\t: bulk\n"), 300u);
    CHECK_EQ(v.write_errors(), 0u);

    v.close();
    CHECK_EQ(count(slurp("/proc/cpuinfo"), "dnuma host"), 0u);
    CHECK(trim(slurp("/sys/devices/system/cpu/online")) == online0);
}

UTEST_MAIN()
