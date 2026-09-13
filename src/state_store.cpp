// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Versioned, integrity-checked, atomically replaced persistence. The loader validates
// the whole file before anything is applied: a corrupt state file can never partially
// mutate the runtime, and dynamic evidence can never silently come back as current.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
// std::fopen is used deliberately: it is the portable, thread-safe-enough primitive for
// the small bounded files this runtime writes, and it keeps the persistence layer free of
// platform-specific file APIs.
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/records_codec.hpp"
#include "federation_observatory/state_store.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

namespace fo {
namespace {

constexpr std::uint32_t kStateFlagsNone = 0;

void write_u32(std::uint8_t* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void write_u64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

std::uint32_t read_u32(const std::uint8_t* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

void encode_watermark(Encoder& out, const PublisherWatermark& value) {
  out.str(value.publisher.value());
  out.u64(value.boot.value());
  out.u64(value.watermark.value());
  out.boolean(value.fenced);
  out.str(value.fenced_reason);
  out.i64(value.last_seen);
}

Result<PublisherWatermark> decode_watermark(Decoder& in) {
  PublisherWatermark value;
  Result<std::string> publisher = in.str();
  if (!publisher.ok()) return publisher.error();
  if (!publisher.value().empty() && !is_valid_identifier(publisher.value())) {
    return Error(ErrorCode::CorruptState, "publisher watermark has an invalid publisher id");
  }
  value.publisher = PublisherId::unchecked(publisher.take());
  Result<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return boot.error();
  value.boot = BootGeneration(boot.value());
  Result<std::uint64_t> watermark = in.u64();
  if (!watermark.ok()) return watermark.error();
  value.watermark = Sequence(watermark.value());
  Result<bool> fenced = in.boolean();
  if (!fenced.ok()) return fenced.error();
  value.fenced = fenced.value();
  Result<std::string> reason = in.str();
  if (!reason.ok()) return reason.error();
  value.fenced_reason = reason.take();
  Result<std::int64_t> last_seen = in.i64();
  if (!last_seen.ok()) return last_seen.error();
  value.last_seen = last_seen.value();
  return value;
}

}  // namespace

bool operator<(const PublisherWatermark& a, const PublisherWatermark& b) noexcept {
  if (a.publisher != b.publisher) return a.publisher < b.publisher;
  return a.boot < b.boot;
}

bool operator<(const AggregateFindingRecord& a, const AggregateFindingRecord& b) noexcept {
  if (a.kind != b.kind) return a.kind < b.kind;
  if (a.subject != b.subject) return a.subject < b.subject;
  return a.digest < b.digest;
}

namespace {

/// Strip everything dynamic from a cluster before it is written, and force the
/// restored-currentness marker. A cluster read back from disk is structurally known and
/// dynamically unknown; anything else would be a lie the loader could not detect.
void normalize_cluster_for_persistence(ClusterRecord& cluster) {
  cluster.capacity_pools.clear();
  cluster.readiness = Readiness::Unknown;
  cluster.currentness = cluster.currentness == Currentness::Retired ? Currentness::Retired
                                                                   : Currentness::RevalidationRequired;
}

[[nodiscard]] bool durable_state_is_normalized(const DurableState& state, std::string& why) {
  for (const ClusterRecord& cluster : state.clusters) {
    if (!cluster.capacity_pools.empty()) {
      why = "cluster " + cluster.id.value() + " carries dynamic capacity pools";
      return false;
    }
    if (cluster.readiness != Readiness::Unknown && cluster.currentness != Currentness::Retired) {
      why = "cluster " + cluster.id.value() + " carries a live readiness value";
      return false;
    }
    if (cluster.currentness == Currentness::Current || cluster.currentness == Currentness::Stale) {
      why = "cluster " + cluster.id.value() + " carries non-structural currentness";
      return false;
    }
  }
  for (const FederationRecord& federation : state.federations) {
    if (federation.currentness == Currentness::Current) {
      why = "federation " + federation.id.value() + " is marked current in a durable file";
      return false;
    }
  }
  for (const SiteRecord& site : state.sites) {
    if (site.currentness == Currentness::Current) {
      why = "site " + site.id.value() + " is marked current in a durable file";
      return false;
    }
  }
  for (const PlacementRecord& placement : state.placements) {
    if (placement.currentness == Currentness::Current) {
      why = "placement " + placement.id.value() + " is marked current in a durable file";
      return false;
    }
  }
  for (const MigrationRecord& migration : state.migrations) {
    if (migration.currentness == Currentness::Current) {
      why = "migration " + migration.id.value() + " is marked current in a durable file";
      return false;
    }
  }
  for (const PortabilityAssessment& assessment : state.portability) {
    if (assessment.currentness == Currentness::Current) {
      why = "portability record for workload " + assessment.workload.value() +
            " is marked current in a durable file";
      return false;
    }
  }
  return true;
}

/// Bring a state into the shape the persistence format requires: structural only.
/// Clusters lose their dynamic evidence and every historical record loses its claim to
/// be current. Both the writer and the validator go through this, so a state that is
/// accepted on save is byte-identical to the state a loader will accept.
void normalize_for_persistence(DurableState& state) {
  state.format_version = kStateFormatVersion;
  for (ClusterRecord& cluster : state.clusters) {
    normalize_cluster_for_persistence(cluster);
  }
  for (FederationRecord& federation : state.federations) {
    if (federation.currentness == Currentness::Current) {
      federation.currentness = Currentness::Stale;
    }
  }
  for (SiteRecord& site : state.sites) {
    if (site.currentness == Currentness::Current) {
      site.currentness = Currentness::Stale;
    }
  }
  for (PlacementRecord& placement : state.placements) {
    if (placement.currentness == Currentness::Current) {
      placement.currentness = Currentness::Stale;
    }
  }
  for (MigrationRecord& migration : state.migrations) {
    if (migration.currentness == Currentness::Current) {
      migration.currentness = Currentness::Stale;
    }
  }
  for (PortabilityAssessment& assessment : state.portability) {
    if (assessment.currentness == Currentness::Current) {
      assessment.currentness = Currentness::Stale;
    }
  }
}

}  // namespace

Status DurableState::validate(const Bounds& bounds) const {
  if (format_version != kStateFormatVersion) {
    return fail(ErrorCode::UnsupportedVersion, "unsupported durable state format version",
                std::to_string(format_version));
  }
  const struct CountCheck {
    std::size_t value;
    std::size_t limit;
    const char* name;
  } counts[] = {
      {federations.size(), bounds.max_federations, "federations"},
      {sites.size(), bounds.max_sites, "sites"},
      {clusters.size(), bounds.max_clusters, "clusters"},
      {accelerator_classes.size(), bounds.max_accelerator_classes, "accelerator_classes"},
      {runtimes.size(), bounds.max_runtimes, "runtimes"},
      {backends.size(), bounds.max_backends, "backends"},
      {domains.size(), bounds.max_domains, "domains"},
      {policies.size(), bounds.max_policies, "policies"},
      {artifacts.size(), bounds.max_artifacts, "artifacts"},
      {workload_classes.size(), bounds.max_workload_classes, "workload_classes"},
      {workloads.size(), bounds.max_workloads, "workloads"},
      {placements.size(), bounds.max_placement_history, "placements"},
      {migrations.size(), bounds.max_migration_history, "migrations"},
      {portability.size(), bounds.max_portability_records, "portability"},
      {publisher_watermarks.size(), bounds.max_publishers, "publisher_watermarks"},
      {aggregate_findings.size(), bounds.max_aggregate_findings, "aggregate_findings"},
  };
  for (const CountCheck& check : counts) {
    if (check.value > check.limit) {
      return fail(ErrorCode::BoundExceeded, "durable state exceeds a configured bound", check.name);
    }
  }

  const auto unique_ids = [](const auto& collection, const char* what) -> Status {
    std::vector<std::string> ids;
    ids.reserve(collection.size());
    for (const auto& record : collection) {
      ids.push_back(record.id.value());
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return fail(ErrorCode::CorruptState, std::string("duplicate identifier in ") + what);
    }
    return Status::success();
  };

  Status s = unique_ids(federations, "federations");
  if (!s.ok()) return s;
  s = unique_ids(sites, "sites");
  if (!s.ok()) return s;
  s = unique_ids(clusters, "clusters");
  if (!s.ok()) return s;
  s = unique_ids(accelerator_classes, "accelerator_classes");
  if (!s.ok()) return s;
  s = unique_ids(runtimes, "runtimes");
  if (!s.ok()) return s;
  s = unique_ids(backends, "backends");
  if (!s.ok()) return s;
  s = unique_ids(domains, "domains");
  if (!s.ok()) return s;
  s = unique_ids(policies, "policies");
  if (!s.ok()) return s;
  s = unique_ids(artifacts, "artifacts");
  if (!s.ok()) return s;
  s = unique_ids(workload_classes, "workload_classes");
  if (!s.ok()) return s;
  s = unique_ids(workloads, "workloads");
  if (!s.ok()) return s;
  s = unique_ids(placements, "placements");
  if (!s.ok()) return s;
  s = unique_ids(migrations, "migrations");
  if (!s.ok()) return s;

  {
    std::vector<PublisherWatermark> watermarks = publisher_watermarks;
    std::sort(watermarks.begin(), watermarks.end());
    for (std::size_t i = 1; i < watermarks.size(); ++i) {
      if (watermarks[i - 1].publisher == watermarks[i].publisher &&
          watermarks[i - 1].boot == watermarks[i].boot) {
        return fail(ErrorCode::CorruptState, "duplicate publisher watermark",
                    watermarks[i].publisher.value());
      }
    }
  }

  for (const FederationRecord& federation : federations) {
    const Status v = federation.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid federation record", v.error().to_string());
    }
  }
  for (const SiteRecord& site : sites) {
    const Status v = site.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid site record", v.error().to_string());
    }
    if (std::none_of(federations.begin(), federations.end(),
                     [&site](const FederationRecord& f) { return f.id == site.federation; })) {
      return fail(ErrorCode::CorruptState, "site references an unknown federation", site.id.value());
    }
  }
  for (const ClusterRecord& cluster : clusters) {
    const Status v = cluster.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid cluster record", v.error().to_string());
    }
    if (std::none_of(federations.begin(), federations.end(), [&cluster](const FederationRecord& f) {
          return f.id == cluster.federation;
        })) {
      return fail(ErrorCode::CorruptState, "cluster references an unknown federation",
                  cluster.id.value());
    }
    if (std::none_of(sites.begin(), sites.end(),
                     [&cluster](const SiteRecord& s) { return s.id == cluster.site; })) {
      return fail(ErrorCode::CorruptState, "cluster references an unknown site", cluster.id.value());
    }
  }
  for (const ClusterRecord& cluster : clusters) {
    for (const AcceleratorClassId& id : cluster.accelerator_classes) {
      if (std::none_of(accelerator_classes.begin(), accelerator_classes.end(),
                       [&id](const AcceleratorClassRecord& a) { return a.id == id; })) {
        return fail(ErrorCode::CorruptState, "cluster references an unknown accelerator class",
                    cluster.id.value() + " " + id.value());
      }
    }
    for (const RuntimeId& id : cluster.runtimes) {
      if (std::none_of(runtimes.begin(), runtimes.end(),
                       [&id](const RuntimeRecord& r) { return r.id == id; })) {
        return fail(ErrorCode::CorruptState, "cluster references an unknown runtime",
                    cluster.id.value() + " " + id.value());
      }
    }
  }
  for (const WorkloadRecord& workload : workloads) {
    const Status v = workload.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid workload record", v.error().to_string());
    }
    if (std::none_of(workload_classes.begin(), workload_classes.end(),
                     [&workload](const WorkloadClassRecord& c) { return c.id == workload.workload_class; })) {
      return fail(ErrorCode::CorruptState, "workload references an unknown workload class",
                  workload.id.value());
    }
    if (!workload.artifact.empty() &&
        std::none_of(artifacts.begin(), artifacts.end(),
                     [&workload](const ArtifactRecord& a) { return a.id == workload.artifact; })) {
      return fail(ErrorCode::CorruptState, "workload references an unknown artifact",
                  workload.id.value());
    }
    if (!workload.policy.empty() &&
        std::none_of(policies.begin(), policies.end(),
                     [&workload](const PolicyRecord& p) { return p.id == workload.policy; })) {
      return fail(ErrorCode::CorruptState, "workload references an unknown policy",
                  workload.id.value());
    }
  }
  for (const PlacementRecord& placement : placements) {
    const Status v = placement.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid placement record", v.error().to_string());
    }
    if (std::none_of(workloads.begin(), workloads.end(),
                     [&placement](const WorkloadRecord& w) { return w.id == placement.workload; })) {
      return fail(ErrorCode::CorruptState, "placement references an unknown workload",
                  placement.id.value());
    }
    if (std::none_of(clusters.begin(), clusters.end(),
                     [&placement](const ClusterRecord& c) { return c.id == placement.selected; })) {
      return fail(ErrorCode::CorruptState, "placement references an unknown selected cluster",
                  placement.id.value());
    }
    for (const CandidateObservation& candidate : placement.candidates) {
      if (std::none_of(clusters.begin(), clusters.end(), [&candidate](const ClusterRecord& c) {
            return c.id == candidate.cluster;
          })) {
        return fail(ErrorCode::CorruptState, "placement candidate references an unknown cluster",
                    placement.id.value() + " " + candidate.cluster.value());
      }
    }
  }
  for (const MigrationRecord& migration : migrations) {
    const Status v = migration.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid migration record", v.error().to_string());
    }
    if (std::none_of(workloads.begin(), workloads.end(),
                     [&migration](const WorkloadRecord& w) { return w.id == migration.workload; })) {
      return fail(ErrorCode::CorruptState, "migration references an unknown workload",
                  migration.id.value());
    }
    const auto known_cluster = [this](const ClusterId& id) {
      return std::any_of(clusters.begin(), clusters.end(),
                         [&id](const ClusterRecord& c) { return c.id == id; });
    };
    if (!known_cluster(migration.source) || !known_cluster(migration.destination)) {
      return fail(ErrorCode::CorruptState, "migration references an unknown cluster",
                  migration.id.value());
    }
  }
  for (const PortabilityAssessment& assessment : portability) {
    const Status v = assessment.validate();
    if (!v.ok()) {
      return fail(ErrorCode::CorruptState, "invalid portability assessment", v.error().to_string());
    }
    if (std::none_of(workloads.begin(), workloads.end(), [&assessment](const WorkloadRecord& w) {
          return w.id == assessment.workload;
        })) {
      return fail(ErrorCode::CorruptState, "portability record references an unknown workload",
                  assessment.workload.value());
    }
  }

  std::string why;
  if (!durable_state_is_normalized(*this, why)) {
    return fail(ErrorCode::CorruptState,
                "durable state carries dynamic evidence that must not be restored as current",
                why);
  }
  return Status::success();
}

std::string DurableState::digest() const {
  const Result<std::vector<std::uint8_t>> encoded = encode_durable_state(*this, default_bounds());
  if (!encoded.ok()) {
    return std::string("encode-failed");
  }
  return hex_digest64(fnv1a64(std::string_view(
      reinterpret_cast<const char*>(encoded.value().data()), encoded.value().size())));
}

std::string DurableState::summary() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"format_version", std::to_string(format_version)});
  rows.push_back({"written_by_epoch", written_by_epoch.is_set() ? written_by_epoch.to_string() : "-"});
  rows.push_back({"save_sequence", std::to_string(save_sequence)});
  rows.push_back({"saved_at", format_timestamp(saved_at)});
  rows.push_back({"federations", std::to_string(federations.size())});
  rows.push_back({"sites", std::to_string(sites.size())});
  rows.push_back({"clusters", std::to_string(clusters.size())});
  rows.push_back({"accelerator_classes", std::to_string(accelerator_classes.size())});
  rows.push_back({"runtimes", std::to_string(runtimes.size())});
  rows.push_back({"backends", std::to_string(backends.size())});
  rows.push_back({"domains", std::to_string(domains.size())});
  rows.push_back({"policies", std::to_string(policies.size())});
  rows.push_back({"artifacts", std::to_string(artifacts.size())});
  rows.push_back({"workload_classes", std::to_string(workload_classes.size())});
  rows.push_back({"workloads", std::to_string(workloads.size())});
  rows.push_back({"placements", std::to_string(placements.size())});
  rows.push_back({"migrations", std::to_string(migrations.size())});
  rows.push_back({"portability", std::to_string(portability.size())});
  rows.push_back({"publisher_watermarks", std::to_string(publisher_watermarks.size())});
  rows.push_back({"aggregate_findings", std::to_string(aggregate_findings.size())});
  rows.push_back({"digest", digest()});
  return render_table({"field", "value"}, rows, "");
}

Result<std::vector<std::uint8_t>> encode_durable_state(const DurableState& state,
                                                       const Bounds& bounds) {
  DurableState normalized = state;
  normalize_for_persistence(normalized);
  std::sort(normalized.placements.begin(), normalized.placements.end(),
            [](const PlacementRecord& a, const PlacementRecord& b) {
              if (a.stamp.observed_at != b.stamp.observed_at) {
                return a.stamp.observed_at < b.stamp.observed_at;
              }
              return a.id < b.id;
            });
  std::sort(normalized.migrations.begin(), normalized.migrations.end(),
            [](const MigrationRecord& a, const MigrationRecord& b) {
              if (a.stamp.observed_at != b.stamp.observed_at) {
                return a.stamp.observed_at < b.stamp.observed_at;
              }
              return a.id < b.id;
            });
  std::sort(normalized.publisher_watermarks.begin(), normalized.publisher_watermarks.end());
  std::sort(normalized.aggregate_findings.begin(), normalized.aggregate_findings.end());

  Encoder payload;
  payload.u32(kStateFormatVersion);
  payload.u64(normalized.written_by_epoch.value());
  payload.u64(normalized.save_sequence);
  payload.i64(normalized.saved_at);

  const auto write_records = [&payload](const auto& collection, auto encode) {
    payload.u32(static_cast<std::uint32_t>(collection.size()));
    for (const auto& record : collection) {
      encode(payload, record);
    }
  };

  write_records(normalized.federations, codec::encode_federation);
  write_records(normalized.sites, codec::encode_site);
  write_records(normalized.clusters, codec::encode_cluster);
  write_records(normalized.accelerator_classes, codec::encode_accelerator_class);
  write_records(normalized.runtimes, codec::encode_runtime);
  write_records(normalized.backends, codec::encode_backend);
  write_records(normalized.domains, codec::encode_domain);
  write_records(normalized.policies, codec::encode_policy);
  write_records(normalized.artifacts, codec::encode_artifact);
  write_records(normalized.workload_classes, codec::encode_workload_class);
  write_records(normalized.workloads, codec::encode_workload);
  write_records(normalized.placements, codec::encode_placement);
  write_records(normalized.migrations, codec::encode_migration);
  write_records(normalized.portability, codec::encode_portability);
  write_records(normalized.publisher_watermarks, encode_watermark);
  payload.u32(static_cast<std::uint32_t>(normalized.aggregate_findings.size()));
  for (const AggregateFindingRecord& finding : normalized.aggregate_findings) {
    payload.str(finding.kind);
    payload.str(finding.subject);
    payload.str(finding.digest);
    payload.i64(finding.generated_at);
    payload.u64(finding.snapshot_generation.value());
    payload.u8(static_cast<std::uint8_t>(finding.precision));
    payload.u8(static_cast<std::uint8_t>(finding.evidence_class));
  }

  if (payload.failed()) {
    return payload.error();
  }
  const std::vector<std::uint8_t>& body = payload.bytes();
  if (body.size() > bounds.max_persistence_bytes) {
    return Error(ErrorCode::BoundExceeded, "durable state exceeds the configured persistence bound",
                 std::to_string(body.size()));
  }

  std::vector<std::uint8_t> image(kStateHeaderSize + body.size(), 0);
  std::memcpy(image.data(), kStateMagic, sizeof(kStateMagic));
  write_u32(image.data() + 8, kStateFormatVersion);
  write_u32(image.data() + 12, kStateFlagsNone);
  write_u64(image.data() + 16, static_cast<std::uint64_t>(body.size()));
  write_u32(image.data() + 24, crc32(body.data(), body.size()));
  write_u32(image.data() + 28, 0);
  write_u64(image.data() + 32, normalized.save_sequence);
  write_u64(image.data() + 40, normalized.written_by_epoch.value());
  std::memcpy(image.data() + kStateHeaderSize, body.data(), body.size());
  return image;
}

Result<DurableState> decode_durable_state(const std::uint8_t* data, std::size_t length,
                                          const Bounds& bounds) {
  if (data == nullptr || length == 0) {
    return Error(ErrorCode::CorruptState, "durable state is empty");
  }
  if (length < kStateHeaderSize) {
    return Error(ErrorCode::CorruptState, "durable state is truncated before the header");
  }
  if (std::memcmp(data, kStateMagic, sizeof(kStateMagic)) != 0) {
    return Error(ErrorCode::CorruptState, "durable state magic does not match");
  }
  if (length > bounds.max_persistence_bytes) {
    return Error(ErrorCode::BoundExceeded, "durable state exceeds the configured persistence bound");
  }
  const std::uint32_t version = read_u32(data + 8);
  if (version != kStateFormatVersion) {
    return Error(ErrorCode::UnsupportedVersion, "unsupported durable state format version",
                 std::to_string(version));
  }
  if (read_u32(data + 12) != kStateFlagsNone) {
    return Error(ErrorCode::CorruptState, "durable state carries unknown flags");
  }
  if (read_u32(data + 28) != 0) {
    return Error(ErrorCode::CorruptState, "durable state reserved field is not zero");
  }
  const std::uint64_t payload_bytes = read_u64(data + 16);
  if (payload_bytes != length - kStateHeaderSize) {
    return Error(ErrorCode::CorruptState, "durable state payload length does not match the header",
                 "declared=" + std::to_string(payload_bytes) +
                     " actual=" + std::to_string(length - kStateHeaderSize));
  }
  const std::uint32_t expected_crc = read_u32(data + 24);
  const std::uint32_t actual_crc = crc32(data + kStateHeaderSize, static_cast<std::size_t>(payload_bytes));
  if (expected_crc != actual_crc) {
    return Error(ErrorCode::IntegrityFailure, "durable state checksum does not match",
                 "expected=" + hex_digest64(expected_crc) + " actual=" + hex_digest64(actual_crc));
  }

  Decoder in(data + kStateHeaderSize, static_cast<std::size_t>(payload_bytes),
             bounds.max_persistence_bytes);
  DurableState state;
  Result<std::uint32_t> payload_version = in.u32();
  if (!payload_version.ok()) return payload_version.error();
  if (payload_version.value() != kStateFormatVersion) {
    return Error(ErrorCode::UnsupportedVersion, "unsupported durable payload version");
  }
  Result<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return epoch.error();
  state.written_by_epoch = CoordinatorEpoch(epoch.value());
  Result<std::uint64_t> save_sequence = in.u64();
  if (!save_sequence.ok()) return save_sequence.error();
  state.save_sequence = save_sequence.value();
  Result<std::int64_t> saved_at = in.i64();
  if (!saved_at.ok()) return saved_at.error();
  state.saved_at = saved_at.value();
  state.format_version = kStateFormatVersion;

  const auto read_count = [&in](std::size_t limit, const char* what) -> Result<std::uint32_t> {
    Result<std::uint32_t> count = in.u32();
    if (!count.ok()) {
      return count.error();
    }
    if (count.value() > limit) {
      return Error(ErrorCode::BoundExceeded, std::string("durable state declares too many ") + what,
                   std::to_string(count.value()));
    }
    return count.value();
  };

  const auto read_records = [&in, &read_count](auto& collection, std::size_t limit,
                                               const char* what, auto decode) -> Status {
    Result<std::uint32_t> count = read_count(limit, what);
    if (!count.ok()) {
      return count.error();
    }
    collection.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
      auto record = decode(in);
      if (!record.ok()) {
        return record.error();
      }
      collection.push_back(record.take());
    }
    return Status::success();
  };

  Status s = read_records(state.federations, bounds.max_federations, "federations",
                          codec::decode_federation);
  if (!s.ok()) return s.error();
  s = read_records(state.sites, bounds.max_sites, "sites", codec::decode_site);
  if (!s.ok()) return s.error();
  s = read_records(state.clusters, bounds.max_clusters, "clusters", codec::decode_cluster);
  if (!s.ok()) return s.error();
  s = read_records(state.accelerator_classes, bounds.max_accelerator_classes, "accelerator classes",
                   codec::decode_accelerator_class);
  if (!s.ok()) return s.error();
  s = read_records(state.runtimes, bounds.max_runtimes, "runtimes", codec::decode_runtime);
  if (!s.ok()) return s.error();
  s = read_records(state.backends, bounds.max_backends, "backends", codec::decode_backend);
  if (!s.ok()) return s.error();
  s = read_records(state.domains, bounds.max_domains, "domains", codec::decode_domain);
  if (!s.ok()) return s.error();
  s = read_records(state.policies, bounds.max_policies, "policies", codec::decode_policy);
  if (!s.ok()) return s.error();
  s = read_records(state.artifacts, bounds.max_artifacts, "artifacts", codec::decode_artifact);
  if (!s.ok()) return s.error();
  s = read_records(state.workload_classes, bounds.max_workload_classes, "workload classes",
                   codec::decode_workload_class);
  if (!s.ok()) return s.error();
  s = read_records(state.workloads, bounds.max_workloads, "workloads", codec::decode_workload);
  if (!s.ok()) return s.error();
  s = read_records(state.placements, bounds.max_placement_history, "placements",
                   codec::decode_placement);
  if (!s.ok()) return s.error();
  s = read_records(state.migrations, bounds.max_migration_history, "migrations",
                   codec::decode_migration);
  if (!s.ok()) return s.error();
  s = read_records(state.portability, bounds.max_portability_records, "portability records",
                   codec::decode_portability);
  if (!s.ok()) return s.error();

  Result<std::uint32_t> watermark_count = read_count(bounds.max_publishers, "publisher watermarks");
  if (!watermark_count.ok()) return watermark_count.error();
  for (std::uint32_t i = 0; i < watermark_count.value(); ++i) {
    Result<PublisherWatermark> watermark = decode_watermark(in);
    if (!watermark.ok()) return watermark.error();
    state.publisher_watermarks.push_back(watermark.take());
  }

  Result<std::uint32_t> finding_count = read_count(bounds.max_aggregate_findings,
                                                   "aggregate findings");
  if (!finding_count.ok()) return finding_count.error();
  for (std::uint32_t i = 0; i < finding_count.value(); ++i) {
    AggregateFindingRecord finding;
    Result<std::string> kind = in.str();
    if (!kind.ok()) return kind.error();
    finding.kind = kind.take();
    Result<std::string> subject = in.str();
    if (!subject.ok()) return subject.error();
    finding.subject = subject.take();
    Result<std::string> digest = in.str();
    if (!digest.ok()) return digest.error();
    finding.digest = digest.take();
    Result<std::int64_t> generated_at = in.i64();
    if (!generated_at.ok()) return generated_at.error();
    finding.generated_at = generated_at.value();
    Result<std::uint64_t> snapshot_generation = in.u64();
    if (!snapshot_generation.ok()) return snapshot_generation.error();
    finding.snapshot_generation = SnapshotGeneration(snapshot_generation.value());
    Result<std::uint8_t> precision = in.u8();
    if (!precision.ok()) return precision.error();
    if (precision.value() > static_cast<std::uint8_t>(Precision::Unknown)) {
      return Error(ErrorCode::CorruptState, "durable state carries an invalid precision");
    }
    finding.precision = static_cast<Precision>(precision.value());
    Result<std::uint8_t> cls = in.u8();
    if (!cls.ok()) return cls.error();
    if (cls.value() > static_cast<std::uint8_t>(EvidenceClass::Unknown)) {
      return Error(ErrorCode::CorruptState, "durable state carries an invalid evidence class");
    }
    finding.evidence_class = static_cast<EvidenceClass>(cls.value());
    state.aggregate_findings.push_back(std::move(finding));
  }

  if (!in.exhausted()) {
    return Error(ErrorCode::CorruptState, "durable state carries trailing bytes",
                 std::to_string(in.remaining()));
  }

  const Status valid = state.validate(bounds);
  if (!valid.ok()) {
    return valid.error();
  }
  return state;
}

Status write_file(const std::string& path, const std::uint8_t* data, std::size_t length) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return fail(ErrorCode::Internal, "could not open file for writing", path);
  }
  std::size_t written = 0;
  if (length != 0) {
    written = std::fwrite(data, 1, length, file);
  }
  const int flush_result = std::fflush(file);
  const int close_result = std::fclose(file);
  if (written != length || flush_result != 0 || close_result != 0) {
    return fail(ErrorCode::Internal, "could not write the complete file", path);
  }
  return Status::success();
}

Result<std::vector<std::uint8_t>> read_file_bounded(const std::string& path, std::size_t max_bytes) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return Error(ErrorCode::NotFound, "could not open file for reading", path);
  }
  std::vector<std::uint8_t> buffer;
  std::uint8_t chunk[64 * 1024];
  for (;;) {
    const std::size_t read = std::fread(chunk, 1, sizeof(chunk), file);
    if (read != 0) {
      if (buffer.size() + read > max_bytes) {
        std::fclose(file);
        return Error(ErrorCode::BoundExceeded, "file exceeds the configured bound", path);
      }
      buffer.insert(buffer.end(), chunk, chunk + read);
    }
    if (read < sizeof(chunk)) {
      if (std::ferror(file) != 0) {
        std::fclose(file);
        return Error(ErrorCode::Internal, "read error", path);
      }
      break;
    }
  }
  std::fclose(file);
  return buffer;
}

Status atomic_replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      0) {
    return fail(ErrorCode::Internal, "atomic replace failed",
                from + " -> " + to + " (windows error " + std::to_string(::GetLastError()) + ")");
  }
  return Status::success();
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return fail(ErrorCode::Internal, "atomic replace failed", from + " -> " + to);
  }
  return Status::success();
#endif
}

Status remove_file_if_present(const std::string& path) {
  if (std::remove(path.c_str()) == 0) {
    return Status::success();
  }
  return Status::success();
}

Status save_durable_state(const DurableState& state, const std::string& path, const Bounds& bounds) {
  DurableState normalized = state;
  normalize_for_persistence(normalized);
  const Status valid = normalized.validate(bounds);
  if (!valid.ok()) {
    return valid.error();
  }
  const Result<std::vector<std::uint8_t>> image = encode_durable_state(normalized, bounds);
  if (!image.ok()) {
    return image.error();
  }
  const std::string temporary = path + ".tmp";
  const Status written = write_file(temporary, image.value().data(), image.value().size());
  if (!written.ok()) {
    const Status cleaned = remove_file_if_present(temporary);
    (void)cleaned;
    return written;
  }
  const Status replaced = atomic_replace_file(temporary, path);
  if (!replaced.ok()) {
    const Status cleaned = remove_file_if_present(temporary);
    (void)cleaned;
    return replaced;
  }
  return Status::success();
}

Result<DurableState> load_durable_state(const std::string& path, const Bounds& bounds) {
  const Result<std::vector<std::uint8_t>> image = read_file_bounded(path, bounds.max_persistence_bytes);
  if (!image.ok()) {
    return image.error();
  }
  return decode_durable_state(image.value().data(), image.value().size(), bounds);
}

}  // namespace fo
