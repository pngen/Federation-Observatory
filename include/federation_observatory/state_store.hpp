// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"

namespace fo {

/// File magic. A file that does not begin with exactly these bytes is not ours.
inline constexpr char kStateMagic[8] = {'F', 'O', 'S', 'T', 'A', 'T', 'E', '1'};
/// Persistence format revision. Loaders reject anything they do not understand.
inline constexpr std::uint32_t kStateFormatVersion = 1;
/// Fixed size of the state file header, in bytes.
inline constexpr std::size_t kStateHeaderSize = 48;
/// Maximum nesting/collection depth accepted by the loader.
inline constexpr std::uint32_t kStateMaxCollectionDepth = 8;

/// Replay-prevention watermark for one publisher boot identity.
struct FO_API PublisherWatermark {
  PublisherId publisher;
  BootGeneration boot;
  Sequence watermark;
  bool fenced = false;
  std::string fenced_reason;
  TimestampNanos last_seen = 0;

  friend bool operator<(const PublisherWatermark& a, const PublisherWatermark& b) noexcept;
};

/// A historical aggregate finding retained across restart. Only the identity and
/// digest of the finding survive: the finding itself is recomputed from current
/// evidence so that a restart can never resurrect a stale conclusion as current.
struct FO_API AggregateFindingRecord {
  std::string kind;
  std::string subject;
  std::string digest;
  TimestampNanos generated_at = 0;
  SnapshotGeneration snapshot_generation;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;

  friend bool operator<(const AggregateFindingRecord& a, const AggregateFindingRecord& b) noexcept;
};

/// Everything the runtime is willing to remember across a restart.
///
/// Deliberately excluded: cluster readiness, cluster liveness, publisher liveness,
/// capacity ledgers, placement candidate freshness, and any other dynamic evidence.
/// Cluster capacity pools are persisted as an empty set and cluster currentness is
/// restored as RevalidationRequired.
struct FO_API DurableState {
  std::uint32_t format_version = kStateFormatVersion;
  /// Coordinator epoch of the process that wrote this state.
  CoordinatorEpoch written_by_epoch;
  std::uint64_t save_sequence = 0;
  TimestampNanos saved_at = 0;

  std::vector<FederationRecord> federations;
  std::vector<SiteRecord> sites;
  std::vector<ClusterRecord> clusters;
  std::vector<AcceleratorClassRecord> accelerator_classes;
  std::vector<RuntimeRecord> runtimes;
  std::vector<BackendRecord> backends;
  std::vector<DomainRecord> domains;
  std::vector<PolicyRecord> policies;
  std::vector<ArtifactRecord> artifacts;
  std::vector<WorkloadClassRecord> workload_classes;
  std::vector<WorkloadRecord> workloads;
  std::vector<PlacementRecord> placements;
  std::vector<MigrationRecord> migrations;
  std::vector<PortabilityAssessment> portability;
  std::vector<PublisherWatermark> publisher_watermarks;
  std::vector<AggregateFindingRecord> aggregate_findings;

  /// Full semantic validation: unique identities, resolvable cross references,
  /// consistent generation bindings, closed capacity accounting, valid lifecycle
  /// states. Any failure rejects the entire state; nothing is applied partially.
  [[nodiscard]] Status validate(const Bounds& bounds) const;
  /// Canonical digest of the whole durable state.
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string summary() const;
};

/// Serialize and atomically replace p path. The temporary file is written first and
/// renamed over the target only after the payload has been fully written and
/// checksummed, so a crash can never leave a half-written state file.
[[nodiscard]] FO_API Status save_durable_state(const DurableState& state, const std::string& path,
                                               const Bounds& bounds);

/// Load and fully validate p path. Returns an Error on any defect; the caller must
/// treat the state as unusable rather than partially applying it.
[[nodiscard]] FO_API Result<DurableState> load_durable_state(const std::string& path,
                                                             const Bounds& bounds);

/// Decode from an in-memory image. Used by the adversarial persistence tests.
[[nodiscard]] FO_API Result<DurableState> decode_durable_state(const std::uint8_t* data,
                                                               std::size_t length,
                                                               const Bounds& bounds);

/// Encode to an in-memory image.
[[nodiscard]] FO_API Result<std::vector<std::uint8_t>> encode_durable_state(
    const DurableState& state, const Bounds& bounds);

/// Atomically replace \p to with \p from, creating no window in which \p to is
/// absent.
[[nodiscard]] FO_API Status atomic_replace_file(const std::string& from, const std::string& to);

/// Read a whole file into memory with a hard byte bound.
[[nodiscard]] FO_API Result<std::vector<std::uint8_t>> read_file_bounded(const std::string& path,
                                                                         std::size_t max_bytes);

/// Write a buffer to a file, truncating any existing content.
[[nodiscard]] FO_API Status write_file(const std::string& path, const std::uint8_t* data,
                                       std::size_t length);

/// Remove a file, reporting NotFound as success.
[[nodiscard]] FO_API Status remove_file_if_present(const std::string& path);

}  // namespace fo
