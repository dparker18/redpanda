// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "pandaproxy/probe.h"

#include "config/configuration.h"
#include "prometheus/prometheus_sanitize.h"
#include "ssx/metrics.h"
#include "ssx/sformat.h"

#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_registration.hh>

namespace pandaproxy {

probe::probe(
  ss::httpd::path_description& path_desc, const ss::sstring& group_name)
  : _request_hist()
  , _metrics()
  , _public_metrics(ssx::metrics::public_metrics_handle) {
    namespace sm = ss::metrics;

    auto operation_label = sm::label("operation");
    std::vector<sm::label_instance> labels{
      operation_label(path_desc.operations.nickname)};

    auto aggregate_labels = std::vector<sm::label>{
      sm::shard_label, operation_label};

    if (!config::shard_local_cfg().disable_metrics()) {
        auto internal_aggregate_labels
          = config::shard_local_cfg().aggregate_metrics()
              ? aggregate_labels
              : std::vector<sm::label>{};

        _metrics.add_group(
          "pandaproxy",
          {sm::make_histogram(
             "request_latency",
             sm::description("Request latency"),
             labels,
             [this] { return _request_hist.seastar_histogram_logform(); })
             .aggregate(internal_aggregate_labels)});
    }

    if (!config::shard_local_cfg().disable_public_metrics()) {
        _public_metrics.add_group(
          group_name,
          {sm::make_histogram(
             "request_latency_seconds",
             sm::description(
               ssx::sformat("Internal latency of request for {}", group_name)),
             labels,
             [this] {
                 return ssx::metrics::report_default_histogram(_request_hist);
             })
             .aggregate(aggregate_labels)});
    }
}

} // namespace pandaproxy