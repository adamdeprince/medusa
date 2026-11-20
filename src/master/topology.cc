// SPDX-License-Identifier: GPL-2.0-only
#include "master/topology.h"

namespace dnuma {

std::vector<NodeAssignEntry> Topology::attach(MachineId machine, const std::string& uuid,
                                              const MachineResources& res) {
    std::vector<NodeAssignEntry> out;
    for (const NumaNodeInfo& n : res.nodes) {
        auto key = std::make_pair(uuid, n.phys_index);
        auto it = index_.find(key);
        NodeId id;
        if (it == index_.end()) {
            id = NodeId(nodes_.size());
            NodeRecord rec;
            rec.id = id;
            rec.logical = int(id); // assigned once, in first-attach order
            rec.machine_uuid = uuid;
            rec.phys_index = n.phys_index;
            nodes_.push_back(rec);
            index_[key] = id;
        } else {
            id = it->second;
        }
        NodeRecord& rec = nodes_[id];
        rec.machine = machine;
        rec.present = true;
        rec.cpus = n.cpus;
        rec.mem_total = n.mem_total;
        rec.mem_free = n.mem_free;
        rec.distances = n.distances;
        NodeAssignEntry e;
        e.phys_index = n.phys_index;
        e.node = id;
        e.logical = rec.logical;
        out.push_back(e);
    }
    return out;
}

void Topology::detach(MachineId machine) {
    for (NodeRecord& r : nodes_)
        if (r.machine == machine) r.present = false;
}

void Topology::update_free(MachineId machine, const std::vector<uint64_t>& mem_free) {
    size_t i = 0;
    for (NodeRecord& r : nodes_) {
        if (r.machine != machine || !r.present) continue;
        if (i < mem_free.size()) r.mem_free = mem_free[i];
        ++i;
    }
}

const NodeRecord* Topology::by_id(NodeId id) const { return id < nodes_.size() ? &nodes_[id] : nullptr; }

const NodeRecord* Topology::by_logical(int logical) const {
    for (const NodeRecord& r : nodes_)
        if (r.logical == logical) return &r;
    return nullptr;
}

std::vector<const NodeRecord*> Topology::nodes_of(MachineId machine) const {
    std::vector<const NodeRecord*> out;
    for (const NodeRecord& r : nodes_)
        if (r.machine == machine && r.present) out.push_back(&r);
    return out;
}

size_t Topology::present_count() const {
    size_t n = 0;
    for (const NodeRecord& r : nodes_) n += r.present ? 1 : 0;
    return n;
}

} // namespace dnuma
