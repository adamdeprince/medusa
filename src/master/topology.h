/* SPDX-License-Identifier: GPL-2.0-only */
// Global NUMA topology: persistent node identities and logical numbering.
//
// Each NUMA domain of each machine gets a NodeId the first time the machine
// attaches (keyed by the machine's stable UUID and the kernel node index).
// NodeIds and user-visible logical numbers are never reassigned to a
// different domain while the master runs, so a slave that detaches and
// reattaches gets its old numbers back and nothing a running process refers
// to is ever renumbered.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/hwprobe.h"
#include "common/types.h"
#include "transport/messages.h"

namespace dnuma {

struct NodeRecord {
    NodeId id = kNoNode;
    int logical = -1;
    MachineId machine = kNoMachine;
    std::string machine_uuid;
    int phys_index = 0;
    bool present = false; // machine currently attached
    std::vector<int> cpus;
    uint64_t mem_total = 0, mem_free = 0;
    std::vector<int> distances;
};

class Topology {
public:
    // Registers (or re-activates) the NUMA domains of a machine.
    std::vector<NodeAssignEntry> attach(MachineId machine, const std::string& uuid,
                                        const MachineResources& res);
    // Marks a machine's nodes absent; identities and numbers are retained.
    void detach(MachineId machine);
    void update_free(MachineId machine, const std::vector<uint64_t>& mem_free);

    const NodeRecord* by_id(NodeId id) const;
    const NodeRecord* by_logical(int logical) const;
    std::vector<const NodeRecord*> nodes_of(MachineId machine) const;
    const std::vector<NodeRecord>& all() const { return nodes_; }
    size_t present_count() const;

private:
    std::vector<NodeRecord> nodes_; // index == NodeId == logical number (both sticky)
    std::map<std::pair<std::string, int>, NodeId> index_;
};

} // namespace dnuma
