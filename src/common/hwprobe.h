/* SPDX-License-Identifier: GPL-2.0-only */
// Hardware and kernel capability discovery. Everything here is read from the
// running system (sysfs, procfs, a throwaway userfaultfd); nothing is assumed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/status.h"

namespace dnuma {

struct NumaNodeInfo {
    int phys_index = 0;          // kernel node number on that machine
    std::vector<int> cpus;       // online CPUs in this node
    uint64_t mem_total = 0;      // bytes
    uint64_t mem_free = 0;       // bytes
    std::vector<int> distances;  // kernel distance row (indexed by position in node list)

    void encode(Writer& w) const;
    bool decode(Reader& r);
};

struct RdmaPortInfo {
    std::string device;      // network interface name, e.g. ib0
    int port = 1;
    std::string link_layer;  // InfiniBand | Ethernet
    std::string state;       // e.g. "4: ACTIVE"
    std::string rate;        // e.g. "40 Gb/sec (4X FDR10)"
    std::string netdev;      // associated netdev, if any
    std::string ipv4;        // address on that netdev, if any
    int numa_node = -1;      // NUMA node physically closest to the HCA
    std::string pci;         // PCI address

    void encode(Writer& w) const;
    bool decode(Reader& r);
};

struct UffdCaps {
    bool available = false;
    uint64_t features = 0;       // UFFD_FEATURE_* supported by the running kernel
    bool dev_node = false;       // /dev/userfaultfd present
    bool missing = false, wp = false, minor_shmem = false, move = false, rwp = false,
         poison = false, thread_id = false;
    std::string summary() const;
};

struct MachineResources {
    std::string hostname;
    std::string kernel;
    std::string cpu_model;
    int sockets = 0;
    int cpus_online = 0;
    std::vector<NumaNodeInfo> nodes;
    std::vector<RdmaPortInfo> rdma_ports;
    uint64_t mem_total = 0, mem_available = 0;
    uint64_t hugepages_total = 0, hugepages_free = 0, hugepage_size = 0;
    uint64_t memlock_limit = 0; // RLIMIT_MEMLOCK (UINT64_MAX = unlimited)
    uint64_t uffd_features = 0;
    bool uffd_dev = false;
    uint64_t xcr0 = 0;           // enabled XSAVE state components
    uint32_t xsave_size = 0;     // bytes needed for the enabled XSAVE area
    std::string cpuinfo;         // /proc/cpuinfo verbatim (only the offered CPUs)
    std::vector<std::string> cpu_flags; // feature flags of the first CPU, sorted

    // Local only (not sent): the description covers a subset of the machine's
    // NUMA nodes, so machine-wide figures are sums over `nodes`.
    bool node_subset = false;

    void encode(Writer& w) const;
    bool decode(Reader& r);
};

// Describes this machine. With `only_nodes` non-empty the description covers
// just those NUMA nodes (kernel node numbers): their CPUs, their memory, and
// the sockets those CPUs belong to. Fails if a listed node does not exist or
// has no online CPU.
Status probe_machine(MachineResources* out, const std::vector<int>& only_nodes = {});
// Refreshes only the per-node free-memory figures (cheap; used for periodic reports).
void refresh_memory(MachineResources* res);
UffdCaps probe_uffd();
// Number of runnable scheduling entities right now (/proc/loadavg, 4th field).
uint32_t runnable_now();
UffdCaps uffd_caps_from_features(uint64_t features, bool dev_node);

// Picks the RDMA port to use: the one whose IPv4 address equals `want_ip` if
// given, otherwise the first ACTIVE InfiniBand-link-layer port with an address,
// otherwise the first ACTIVE port with an address.
const RdmaPortInfo* choose_rdma_port(const MachineResources& res, const std::string& want_ip);

} // namespace dnuma
