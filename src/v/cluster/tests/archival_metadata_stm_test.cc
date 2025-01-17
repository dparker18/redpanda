// Copyright 2022 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "cloud_storage/partition_manifest.h"
#include "cloud_storage/remote.h"
#include "cloud_storage/types.h"
#include "cluster/archival_metadata_stm.h"
#include "cluster/errc.h"
#include "cluster/persisted_stm.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/record.h"
#include "model/timestamp.h"
#include "raft/tests/mux_state_machine_fixture.h"
#include "raft/tests/raft_group_fixture.h"
#include "raft/types.h"
#include "storage/tests/utils/disk_log_builder.h"
#include "test_utils/async.h"
#include "test_utils/http_imposter.h"

#include <seastar/core/io_priority_class.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/noncopyable_function.hh>

#include <boost/test/tools/old/interface.hpp>

#include <chrono>

using namespace std::chrono_literals;

struct archival_metadata_stm_base_fixture
  : mux_state_machine_fixture
  , http_imposter_fixture {
    using mux_state_machine_fixture::start_raft;
    using mux_state_machine_fixture::wait_for_becoming_leader;
    using mux_state_machine_fixture::wait_for_confirmed_leader;

    archival_metadata_stm_base_fixture(
      const archival_metadata_stm_base_fixture&)
      = delete;
    archival_metadata_stm_base_fixture&
    operator=(const archival_metadata_stm_base_fixture&)
      = delete;
    archival_metadata_stm_base_fixture(archival_metadata_stm_base_fixture&&)
      = delete;
    archival_metadata_stm_base_fixture&
    operator=(archival_metadata_stm_base_fixture&&)
      = delete;

    static s3::configuration get_s3_configuration() {
        net::unresolved_address server_addr(
          ss::sstring(httpd_host_name), httpd_port_number);
        s3::configuration conf{
          .uri = s3::access_point_uri(ss::sstring(httpd_host_name)),
          .access_key = cloud_roles::public_key_str("acess-key"),
          .secret_key = cloud_roles::private_key_str("secret-key"),
          .region = cloud_roles::aws_region_name("us-east-1"),
        };
        conf.server_addr = server_addr;
        conf._probe = ss::make_shared<s3::client_probe>(
          net::metrics_disabled::yes,
          "us-east-1",
          ss::sstring(httpd_host_name));
        return conf;
    }

    archival_metadata_stm_base_fixture() {
        // Cloud storage config
        cloud_cfg.start().get();
        cloud_cfg
          .invoke_on_all([](cloud_storage::configuration& cfg) {
              cfg.bucket_name = s3::bucket_name("panda-bucket");
              cfg.metrics_disabled
                = cloud_storage::remote_metrics_disabled::yes;
              cfg.connection_limit = cloud_storage::s3_connection_limit(10);
              cfg.client_config = get_s3_configuration();
          })
          .get();
        // Cloud storage remote api
        cloud_api.start(std::ref(cloud_cfg)).get();
        cloud_api
          .invoke_on_all([](cloud_storage::remote& api) { return api.start(); })
          .get();
    }

    ~archival_metadata_stm_base_fixture() override {
        cloud_api.stop().get();
        cloud_cfg.stop().get();
    }

    ss::sharded<cloud_storage::configuration> cloud_cfg;
    ss::sharded<cloud_storage::remote> cloud_api;
    ss::logger logger{"archival_metadata_stm_test"};
};

struct archival_metadata_stm_fixture : archival_metadata_stm_base_fixture {
    archival_metadata_stm_fixture() {
        // Archival metadata STM
        start_raft();
        archival_stm = std::make_unique<cluster::archival_metadata_stm>(
          _raft.get(), cloud_api.local(), logger);

        archival_stm->start().get();
    }

    ~archival_metadata_stm_fixture() override { archival_stm->stop().get(); }

    std::unique_ptr<cluster::archival_metadata_stm> archival_stm;
};

using cloud_storage::partition_manifest;
using segment_meta = cloud_storage::partition_manifest::segment_meta;
using cloud_storage::segment_name;

FIXTURE_TEST(test_archival_stm_happy_path, archival_metadata_stm_fixture) {
    wait_for_confirmed_leader();
    auto& ntp_cfg = _raft->log_config();
    partition_manifest m(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(0),
        .committed_offset = model::offset(99),
        .archiver_term = model::term_id(1),
      });
    // Replicate add_segment_cmd command that adds segment with offset 0
    archival_stm->add_segments(m, ss::lowres_clock::now() + 10s).get();
    BOOST_REQUIRE(archival_stm->manifest().size() == 1);
    BOOST_REQUIRE(
      archival_stm->manifest().begin()->second.base_offset == model::offset(0));
    BOOST_REQUIRE(
      archival_stm->manifest().begin()->second.committed_offset
      == model::offset(99));
}

FIXTURE_TEST(test_archival_stm_segment_replace, archival_metadata_stm_fixture) {
    wait_for_confirmed_leader();
    auto& ntp_cfg = _raft->log_config();
    partition_manifest m1(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m1.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(0),
        .committed_offset = model::offset(999),
        .archiver_term = model::term_id(1),
      });
    m1.add(
      segment_name("1000-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(1000),
        .committed_offset = model::offset(1999),
        .archiver_term = model::term_id(1),
      });
    m1.advance_insync_offset(model::offset{2});
    // Replicate add_segment_cmd command that adds segment with offset 0
    archival_stm->add_segments(m1, ss::lowres_clock::now() + 10s).get();
    archival_stm->sync(10s).get();
    BOOST_REQUIRE(archival_stm->manifest().size() == 2);
    BOOST_REQUIRE(archival_stm->get_start_offset() == model::offset(0));
    // Manifests are not stictly equal in general but here we can
    // make them to be.
    BOOST_REQUIRE(archival_stm->manifest() == m1);

    // Replace first segment
    partition_manifest m2(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m2.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .is_compacted = true,
        .base_offset = model::offset(0),
        .committed_offset = model::offset(999),
        .archiver_term = model::term_id(1),
      });
    archival_stm->add_segments(m2, ss::lowres_clock::now() + 10s).get();
    archival_stm->sync(10s).get();
    BOOST_REQUIRE(archival_stm->manifest().size() == 2);
    BOOST_REQUIRE(archival_stm->manifest().replaced_segments().size() == 1);
    BOOST_REQUIRE(archival_stm->get_start_offset() == model::offset(0));
}

FIXTURE_TEST(test_snapshot_loading, archival_metadata_stm_base_fixture) {
    start_raft();
    auto& ntp_cfg = _raft->log_config();
    partition_manifest m(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(0),
        .committed_offset = model::offset(99),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("100-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(100),
        .committed_offset = model::offset(199),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("200-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(200),
        .committed_offset = model::offset(299),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("100-1-v1.log"),
      segment_meta{
        .is_compacted = true,
        .base_offset = model::offset(100),
        .committed_offset = model::offset(299),
        .archiver_term = model::term_id(1),
        .sname_format = cloud_storage::segment_name_format::v2,
      });
    m.advance_insync_offset(model::offset{42});

    BOOST_REQUIRE(m.advance_start_offset(model::offset{100}));
    BOOST_REQUIRE_EQUAL(m.get_start_offset().value(), model::offset(100));
    BOOST_REQUIRE_EQUAL(m.get_insync_offset(), model::offset(42));
    cluster::archival_metadata_stm::make_snapshot(ntp_cfg, m, model::offset{42})
      .get();

    cluster::archival_metadata_stm archival_stm(
      _raft.get(), cloud_api.local(), logger);

    archival_stm.start().get();
    wait_for_confirmed_leader();

    {
        std::stringstream s1, s2;
        m.serialize(s1);
        archival_stm.manifest().serialize(s2);
        vlog(logger.info, "original manifest: {}", s1.str());
        vlog(logger.info, "restored manifest: {}", s2.str());
    }

    BOOST_REQUIRE_EQUAL(archival_stm.get_start_offset(), model::offset{100});
    BOOST_REQUIRE(archival_stm.manifest() == m);
    archival_stm.stop().get();
}

FIXTURE_TEST(
  test_archival_stm_segment_truncate, archival_metadata_stm_fixture) {
    wait_for_confirmed_leader();
    auto& ntp_cfg = _raft->log_config();
    partition_manifest m(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(0),
        .committed_offset = model::offset(99),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("100-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(100),
        .committed_offset = model::offset(199),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("200-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(200),
        .committed_offset = model::offset(299),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("300-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(300),
        .committed_offset = model::offset(399),
        .archiver_term = model::term_id(1),
      });
    m.advance_insync_offset(model::offset{4});
    archival_stm->add_segments(m, ss::lowres_clock::now() + 10s).get();
    BOOST_REQUIRE(archival_stm->manifest().size() == 4);
    BOOST_REQUIRE(archival_stm->get_start_offset() == model::offset(0));
    BOOST_REQUIRE(archival_stm->manifest() == m);

    // Truncate the STM, first segment should be added to the backlog
    archival_stm->truncate(model::offset(101), ss::lowres_clock::now() + 10s)
      .get();

    BOOST_REQUIRE_EQUAL(archival_stm->get_start_offset(), model::offset(100));
    auto backlog = archival_stm->get_segments_to_cleanup();
    BOOST_REQUIRE_EQUAL(backlog.size(), 1);
    auto name = cloud_storage::generate_local_segment_name(
      backlog[0].base_offset, backlog[0].segment_term);
    BOOST_REQUIRE(m.get(name) != nullptr);
    BOOST_REQUIRE(backlog[0] == *m.get(name));

    // Truncate the STM, next segment should be added to the backlog
    archival_stm->truncate(model::offset(200), ss::lowres_clock::now() + 10s)
      .get();

    BOOST_REQUIRE_EQUAL(archival_stm->get_start_offset(), model::offset(200));
    backlog = archival_stm->get_segments_to_cleanup();
    BOOST_REQUIRE_EQUAL(backlog.size(), 2);
    for (const auto& it : backlog) {
        auto name = cloud_storage::generate_local_segment_name(
          it.base_offset, it.segment_term);
        BOOST_REQUIRE(m.get(name) != nullptr);
        BOOST_REQUIRE(it == *m.get(name));
    }
}

namespace old {

using namespace cluster;

struct segment
  : public serde::
      envelope<segment, serde::version<0>, serde::compat_version<0>> {
    // ntp_revision is needed to reconstruct full remote path of
    // the segment. Deprecated because ntp_revision is now part of
    // segment_meta.
    model::initial_revision_id ntp_revision_deprecated;
    cloud_storage::segment_name name;
    cloud_storage::partition_manifest::segment_meta meta;
};

struct snapshot
  : public serde::
      envelope<snapshot, serde::version<0>, serde::compat_version<0>> {
    /// List of segments
    std::vector<segment> segments;
};

} // namespace old

std::vector<old::segment>
old_segments_from_manifest(const cloud_storage::partition_manifest& m) {
    std::vector<old::segment> segments;
    segments.reserve(m.size() + m.size());

    for (auto [key, meta] : m) {
        if (meta.ntp_revision == model::initial_revision_id{}) {
            meta.ntp_revision = m.get_revision_id();
        }
        auto name = cloud_storage::generate_local_segment_name(
          key.base_offset, key.term);
        segments.push_back(old::segment{
          .ntp_revision_deprecated = meta.ntp_revision,
          .name = std::move(name),
          .meta = meta});
    }

    std::sort(
      segments.begin(), segments.end(), [](const auto& s1, const auto& s2) {
          return s1.meta.base_offset < s2.meta.base_offset;
      });

    return segments;
}

namespace cluster::details {
class archival_metadata_stm_accessor {
public:
    static ss::future<> persist_snapshot(
      storage::simple_snapshot_manager& mgr, cluster::stm_snapshot&& snapshot) {
        return archival_metadata_stm::persist_snapshot(
          mgr, std::move(snapshot));
    }
};
} // namespace cluster::details

ss::future<> make_old_snapshot(
  const storage::ntp_config& ntp_cfg,
  const cloud_storage::partition_manifest& m,
  model::offset insync_offset) {
    // Create archival_stm_snapshot
    auto segments = old_segments_from_manifest(m);
    iobuf snap_data = serde::to_iobuf(
      old::snapshot{.segments = std::move(segments)});

    auto snapshot = cluster::stm_snapshot::create(
      0, insync_offset, std::move(snap_data));

    storage::simple_snapshot_manager tmp_snapshot_mgr(
      std::filesystem::path(ntp_cfg.work_directory()),
      "archival_metadata.snapshot",
      ss::default_priority_class());

    co_await cluster::details::archival_metadata_stm_accessor::persist_snapshot(
      tmp_snapshot_mgr, std::move(snapshot));
}

FIXTURE_TEST(
  test_archival_metadata_stm_snapshot_version_compatibility,
  archival_metadata_stm_base_fixture) {
    start_raft();
    auto& ntp_cfg = _raft->log_config();
    partition_manifest m(ntp_cfg.ntp(), ntp_cfg.get_initial_revision());
    m.add(
      segment_name("0-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(0),
        .committed_offset = model::offset(99),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("100-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(100),
        .committed_offset = model::offset(199),
        .archiver_term = model::term_id(1),
      });
    m.add(
      segment_name("200-1-v1.log"),
      segment_meta{
        .base_offset = model::offset(200),
        .committed_offset = model::offset(299),
        .archiver_term = model::term_id(1),
      });
    m.advance_insync_offset(model::offset(3));

    make_old_snapshot(ntp_cfg, m, model::offset{3}).get();

    cluster::archival_metadata_stm archival_stm(
      _raft.get(), cloud_api.local(), logger);

    archival_stm.start().get();
    wait_for_confirmed_leader();

    BOOST_REQUIRE(archival_stm.manifest() == m);

    archival_stm.stop().get();
}