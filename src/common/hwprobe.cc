// SPDX-License-Identifier: GPL-2.0-only
#include "common/hwprobe.h"

#include <algorithm>
#include <cstring>
#include <set>

#include <arpa/inet.h>
#include <cpuid.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/userfaultfd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"

namespace dnuma {

namespace {

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        out.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// "Node 0 MemFree:   123 kB" -> bytes
uint64_t meminfo_field(const std::string& text, const std::string& key) {
    size_t pos = text.find(key + ":");
    if (pos == std::string::npos) return 0;
    return strtoull(text.c_str() + pos + key.size() + 1, nullptr, 10) * 1024;
}

std::string ipv4_of(const std::string& ifname) {
    std::string out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || ifname != p->ifa_name) continue;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buf, sizeof buf)) {
            out = buf;
            break;
        }
    }
    freeifaddrs(ifa);
    return out;
}

// Memory of one node that could be given to new work: free pages plus what
// the kernel can drop on demand (the same idea as MemAvailable, which the
// kernel only computes machine-wide).
uint64_t node_available(const std::string& node_meminfo) {
    return meminfo_field(node_meminfo, "MemFree") + meminfo_field(node_meminfo, "Active(file)") +
           meminfo_field(node_meminfo, "Inactive(file)") + meminfo_field(node_meminfo, "SReclaimable");
}

// Keeps only the /proc/cpuinfo blocks of the given CPUs.
std::string filter_cpuinfo(const std::string& text, const std::set<int>& cpus) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find("\n\n", pos);
        end = end == std::string::npos ? text.size() : end + 2;
        std::string block = text.substr(pos, end - pos);
        size_t p = block.find("processor"), colon = p == std::string::npos ? p : block.find(':', p);
        if (colon != std::string::npos && cpus.count(atoi(block.c_str() + colon + 1))) out += block;
        pos = end;
    }
    return out;
}

void read_node(int idx, NumaNodeInfo* n) {
    std::string base = "/sys/devices/system/node/node" + std::to_string(idx);
    n->phys_index = idx;
    n->cpus = parse_cpulist(read_attr(base + "/cpulist"));
    std::string mi;
    if (read_file(base + "/meminfo", &mi).is_ok()) {
        n->mem_total = meminfo_field(mi, "MemTotal");
        n->mem_free = meminfo_field(mi, "MemFree");
    }
    n->distances.clear();
    for (const std::string& d : split(read_attr(base + "/distance"), ' '))
        n->distances.push_back(atoi(d.c_str()));
}

} // namespace

void NumaNodeInfo::encode(Writer& w) const {
    w.i32(phys_index);
    w.i32s(cpus);
    w.u64(mem_total);
    w.u64(mem_free);
    w.i32s(distances);
}
bool NumaNodeInfo::decode(Reader& r) {
    phys_index = r.i32();
    cpus = r.i32s();
    mem_total = r.u64();
    mem_free = r.u64();
    distances = r.i32s();
    return r.ok();
}

void RdmaPortInfo::encode(Writer& w) const {
    w.str(device);
    w.i32(port);
    w.str(link_layer);
    w.str(state);
    w.str(rate);
    w.str(netdev);
    w.str(ipv4);
    w.i32(numa_node);
    w.str(pci);
}
bool RdmaPortInfo::decode(Reader& r) {
    device = r.str();
    port = r.i32();
    link_layer = r.str();
    state = r.str();
    rate = r.str();
    netdev = r.str();
    ipv4 = r.str();
    numa_node = r.i32();
    pci = r.str();
    return r.ok();
}

uint32_t runnable_now() {
    std::string s = read_attr("/proc/loadavg"); // "0.00 0.00 0.00 R/T pid"
    std::vector<std::string> f = split(s, ' ');
    return f.size() >= 4 ? uint32_t(strtoul(f[3].c_str(), nullptr, 10)) : 0;
}

void MachineResources::encode(Writer& w) const {
    w.str(hostname);
    w.str(kernel);
    w.str(cpu_model);
    w.i32(sockets);
    w.i32(cpus_online);
    w.u32(uint32_t(nodes.size()));
    for (const auto& n : nodes) n.encode(w);
    w.u32(uint32_t(rdma_ports.size()));
    for (const auto& p : rdma_ports) p.encode(w);
    w.u64(mem_total);
    w.u64(mem_available);
    w.u64(hugepages_total);
    w.u64(hugepages_free);
    w.u64(hugepage_size);
    w.u64(memlock_limit);
    w.u64(uffd_features);
    w.boolean(uffd_dev);
    w.u64(xcr0);
    w.u32(xsave_size);
    w.str(cpuinfo);
    w.strs(cpu_flags);
}
bool MachineResources::decode(Reader& r) {
    hostname = r.str();
    kernel = r.str();
    cpu_model = r.str();
    sockets = r.i32();
    cpus_online = r.i32();
    uint32_t n = r.count(24);
    nodes.assign(n, {});
    for (auto& x : nodes)
        if (!x.decode(r)) return false;
    n = r.count(28);
    rdma_ports.assign(n, {});
    for (auto& x : rdma_ports)
        if (!x.decode(r)) return false;
    mem_total = r.u64();
    mem_available = r.u64();
    hugepages_total = r.u64();
    hugepages_free = r.u64();
    hugepage_size = r.u64();
    memlock_limit = r.u64();
    uffd_features = r.u64();
    uffd_dev = r.boolean();
    xcr0 = r.u64();
    xsave_size = r.u32();
    cpuinfo = r.str();
    cpu_flags = r.strs();
    return r.ok();
}

UffdCaps uffd_caps_from_features(uint64_t features, bool dev_node) {
    UffdCaps c;
    c.available = true;
    c.features = features;
    c.dev_node = dev_node;
    c.missing = true;
    c.wp = features & UFFD_FEATURE_PAGEFAULT_FLAG_WP;
    c.minor_shmem = features & UFFD_FEATURE_MINOR_SHMEM;
    c.move = features & UFFD_FEATURE_MOVE;
    c.rwp = features & UFFD_FEATURE_RWP;
    c.poison = features & UFFD_FEATURE_POISON;
    c.thread_id = features & UFFD_FEATURE_THREAD_ID;
    return c;
}

std::string UffdCaps::summary() const {
    if (!available) return "unavailable";
    std::string s = "missing";
    if (wp) s += ",wp";
    if (minor_shmem) s += ",minor-shmem";
    if (move) s += ",move";
    if (rwp) s += ",rwp";
    if (poison) s += ",poison";
    if (thread_id) s += ",thread-id";
    if (dev_node) s += ",/dev/userfaultfd";
    return s;
}

UffdCaps probe_uffd() {
    UffdCaps c;
    // A throwaway descriptor: UFFDIO_API with features=0 reports everything
    // the running kernel supports (the handshake can only be done once per fd).
    int fd = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
    if (fd < 0) fd = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    bool dev = access("/dev/userfaultfd", F_OK) == 0;
    if (fd < 0) {
        c.dev_node = dev;
        return c;
    }
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(fd, UFFDIO_API, &api) == 0) c = uffd_caps_from_features(api.features, dev);
    close(fd);
    return c;
}

void refresh_memory(MachineResources* res) {
    uint64_t total = 0, available = 0;
    for (auto& n : res->nodes) {
        std::string mi;
        if (read_file("/sys/devices/system/node/node" + std::to_string(n.phys_index) + "/meminfo", &mi)
                .is_ok()) {
            n.mem_total = meminfo_field(mi, "MemTotal");
            n.mem_free = meminfo_field(mi, "MemFree");
            total += n.mem_total;
            available += node_available(mi);
        }
    }
    std::string mi;
    if (read_file("/proc/meminfo", &mi).is_ok()) {
        res->mem_total = meminfo_field(mi, "MemTotal");
        res->mem_available = meminfo_field(mi, "MemAvailable");
        res->hugepages_free = meminfo_field(mi, "HugePages_Free") / 1024;
    }
    if (res->node_subset) {
        res->mem_total = total;
        res->mem_available = available;
    }
}

Status probe_machine(MachineResources* out, const std::vector<int>& only_nodes) {
    MachineResources r;
    utsname un{};
    if (uname(&un) != 0) return Status::sys("uname");
    r.hostname = un.nodename;
    r.kernel = un.release;

    std::string& cpuinfo = r.cpuinfo;
    if (read_file("/proc/cpuinfo", &cpuinfo).is_ok()) {
        size_t fp = cpuinfo.find("\nflags");
        if (fp != std::string::npos) {
            size_t colon = cpuinfo.find(':', fp), eol = cpuinfo.find('\n', fp + 1);
            if (colon != std::string::npos && eol != std::string::npos) {
                r.cpu_flags = split(cpuinfo.substr(colon + 1, eol - colon - 1), ' ');
                std::sort(r.cpu_flags.begin(), r.cpu_flags.end());
            }
        }
        size_t pos = cpuinfo.find("model name");
        if (pos != std::string::npos) {
            size_t colon = cpuinfo.find(':', pos), eol = cpuinfo.find('\n', pos);
            if (colon != std::string::npos && eol != std::string::npos)
                r.cpu_model = trim(cpuinfo.substr(colon + 1, eol - colon - 1));
        }
    }

    std::vector<int> online = parse_cpulist(read_attr("/sys/devices/system/cpu/online"));

    // NUMA nodes that actually have memory or CPUs, as the kernel exposes them.
    std::string nodes_online = read_attr("/sys/devices/system/node/online");
    std::vector<int> present = parse_cpulist(nodes_online);
    for (int want : only_nodes)
        if (std::find(present.begin(), present.end(), want) == present.end())
            return Status::err(EINVAL, "NUMA node %d does not exist on this machine (nodes: %s)", want,
                               nodes_online.c_str());
    for (int idx : present) {
        if (!only_nodes.empty() && std::find(only_nodes.begin(), only_nodes.end(), idx) == only_nodes.end())
            continue;
        NumaNodeInfo n;
        read_node(idx, &n);
        if (!only_nodes.empty() && n.cpus.empty())
            return Status::err(EINVAL, "NUMA node %d has no online CPU and cannot be offered on its own", idx);
        r.nodes.push_back(std::move(n));
    }
    if (r.nodes.empty()) return Status::err(ENODEV, "no NUMA nodes found in sysfs");
    r.node_subset = !only_nodes.empty();
    if (r.node_subset) {
        std::set<int> offered;
        for (const auto& n : r.nodes) offered.insert(n.cpus.begin(), n.cpus.end());
        online.assign(offered.begin(), offered.end());
        cpuinfo = filter_cpuinfo(cpuinfo, offered);
    }
    r.cpus_online = int(online.size());
    std::set<std::string> packages;
    for (int c : online)
        packages.insert(read_attr("/sys/devices/system/cpu/cpu" + std::to_string(c) +
                                  "/topology/physical_package_id"));
    r.sockets = int(packages.size());

    for (const std::string& dev : list_dir("/sys/class/infiniband")) {
        std::string base = "/sys/class/infiniband/" + dev;
        char real[4096];
        std::string pci;
        if (realpath((base + "/device").c_str(), real)) {
            pci = real;
            pci = pci.substr(pci.rfind('/') + 1);
        }
        int numa = atoi(read_attr(base + "/device/numa_node").c_str());
        std::vector<std::string> netdevs = list_dir(base + "/device/net");
        for (const std::string& port : list_dir(base + "/ports")) {
            RdmaPortInfo p;
            p.device = dev;
            p.port = atoi(port.c_str());
            p.link_layer = read_attr(base + "/ports/" + port + "/link_layer");
            p.state = read_attr(base + "/ports/" + port + "/state");
            p.rate = read_attr(base + "/ports/" + port + "/rate");
            p.numa_node = numa;
            p.pci = pci;
            // Match the netdev by dev_port when several ports share a device.
            for (const std::string& nd : netdevs) {
                std::string dp = read_attr("/sys/class/net/" + nd + "/dev_port");
                if (netdevs.size() == 1 || atoi(dp.c_str()) == p.port - 1) {
                    p.netdev = nd;
                    p.ipv4 = ipv4_of(nd);
                    break;
                }
            }
            r.rdma_ports.push_back(std::move(p));
        }
    }

    refresh_memory(&r); // memory totals (machine-wide, or of the offered nodes)
    std::string mi;
    if (read_file("/proc/meminfo", &mi).is_ok()) {
        r.hugepages_total = meminfo_field(mi, "HugePages_Total") / 1024;
        r.hugepages_free = meminfo_field(mi, "HugePages_Free") / 1024;
        r.hugepage_size = meminfo_field(mi, "Hugepagesize");
    }
    rlimit rl{};
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
        r.memlock_limit = rl.rlim_cur == RLIM_INFINITY ? UINT64_MAX : uint64_t(rl.rlim_cur);

    UffdCaps uc = probe_uffd();
    r.uffd_features = uc.available ? uc.features : 0;
    r.uffd_dev = uc.dev_node;

    // XSAVE layout: execution-state transfer is only valid between machines
    // that enable the same state components.
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c & bit_OSXSAVE)) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        r.xcr0 = uint64_t(hi) << 32 | lo;
        if (__get_cpuid_count(0xd, 0, &a, &b, &c, &d)) r.xsave_size = b;
    }

    *out = std::move(r);
    return Status::ok();
}

const RdmaPortInfo* choose_rdma_port(const MachineResources& res, const std::string& want_ip) {
    const RdmaPortInfo* fallback = nullptr;
    for (const auto& p : res.rdma_ports) {
        if (p.ipv4.empty()) continue;
        if (!want_ip.empty()) {
            if (p.ipv4 == want_ip) return &p;
            continue;
        }
        if (p.state.find("ACTIVE") == std::string::npos) continue;
        if (p.link_layer == "InfiniBand") return &p;
        if (!fallback) fallback = &p;
    }
    return fallback;
}

} // namespace dnuma
