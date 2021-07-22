/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "cluster/scheduling/types.h"
#include "cluster/types.h"
#include "model/fundamental.h"

#include <absl/container/node_hash_map.h>

namespace cluster {

class allocation_state;
/**
 * Allocation node represent a node where partitions may be allocated
 */
class allocation_node {
public:
    using allocation_capacity
      = named_type<uint32_t, struct allocation_node_slot_tag>;

    static constexpr const allocation_capacity core0_extra_weight{2};
    // TODO make configurable
    static constexpr const allocation_capacity max_allocations_per_core{7000};

    allocation_node(
      model::node_id, uint32_t, absl::node_hash_map<ss::sstring, ss::sstring>);

    allocation_node(allocation_node&& o) noexcept = default;
    allocation_node& operator=(allocation_node&&) = delete;
    allocation_node(const allocation_node&) = delete;
    allocation_node& operator=(const allocation_node&) = delete;
    ~allocation_node() = default;

    uint32_t cpus() const { return _weights.size(); }
    model::node_id id() const { return _id; }

    allocation_capacity partition_capacity() const {
        // there might be a situation when node is over assigned, this state is
        // transient and it may be caused by holding allocation units while
        // state is being updated
        return _max_capacity - std::min(_allocated_partitions, _max_capacity);
    }
    void decommission() { _decommissioned = true; }
    void recommission() { _decommissioned = false; }
    bool is_decommissioned() const { return _decommissioned; }

    allocation_capacity allocated_partitions() const {
        return _allocated_partitions;
    }

    bool empty() const {
        return _allocated_partitions == allocation_capacity{0};
    }
    bool is_full() const { return _allocated_partitions >= _max_capacity; }
    allocation_capacity max_capacity() const { return _max_capacity; }
    ss::shard_id allocate();

private:
    friend allocation_state;

    void deallocate(ss::shard_id core);
    void allocate(ss::shard_id core);
    const absl::node_hash_map<ss::sstring, ss::sstring>& machine_labels() const;

    model::node_id _id;
    /// each index is a CPU. A weight is roughly the number of assignments
    std::vector<uint32_t> _weights;
    const allocation_capacity _max_capacity;
    allocation_capacity _allocated_partitions{0};
    /// generated by `rpk` usually in /etc/redpanda/machine_labels.json
    absl::node_hash_map<ss::sstring, ss::sstring> _machine_labels;
    bool _decommissioned = false;

    friend std::ostream& operator<<(std::ostream&, const allocation_node&);
};
} // namespace cluster