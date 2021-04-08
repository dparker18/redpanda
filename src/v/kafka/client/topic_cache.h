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

#include "kafka/protocol/metadata.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "seastarx.h"

#include <seastar/core/future.hh>

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_set.h>

namespace kafka::client {

class topic_cache {
    using leaders_t
      = absl::flat_hash_map<model::topic_partition, model::node_id>;

public:
    topic_cache() = default;
    topic_cache(const topic_cache&) = delete;
    topic_cache(topic_cache&&) = default;
    topic_cache& operator=(topic_cache const&) = delete;
    topic_cache& operator=(topic_cache&&) = delete;
    ~topic_cache() noexcept = default;

    /// \brief Apply the given metadata response.
    ss::future<> apply(std::vector<metadata_response::topic>&& topics);

    /// \brief Obtain the leader for the given topic-partition
    ss::future<model::node_id> leader(model::topic_partition tp) const;

private:
    /// \brief Leaders map a partition to a model::node_id.
    leaders_t _leaders;
};

} // namespace kafka::client
