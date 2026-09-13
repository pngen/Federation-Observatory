// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"

namespace fo {

// Compile-time floors and ceilings. Every runtime bound is clamped into this range so
// that an untrusted configuration can never disable the bound entirely.
inline constexpr std::size_t kMinEvidencePerFinding = 1;
inline constexpr std::size_t kMaxEvidencePerFinding = 64;
inline constexpr std::size_t kDefaultEvidencePerFinding = 16;

inline constexpr std::size_t kMaxCapabilityEntriesPerPublication = 512;
inline constexpr std::size_t kMaxValueSetEntries = 32;
inline constexpr std::size_t kMaxMetadataBytes = 64 * 1024;
inline constexpr std::size_t kMaxStringFieldBytes = 4096;
inline constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
inline constexpr std::size_t kMinFrameBytes = 64;
inline constexpr std::size_t kMaxQueryResponseBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxSnapshotBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxPersistenceBytes = 256ull * 1024 * 1024;
inline constexpr std::size_t kMaxRejectionReasons = 32;
inline constexpr std::size_t kMaxCandidatesPerPlacement = 4096;
inline constexpr std::size_t kMaxStageEventsPerMigration = 256;
inline constexpr std::size_t kMaxClusterPerSite = 4096;
inline constexpr std::size_t kMaxMigrationHistoryPerWorkload = 4096;
inline constexpr std::size_t kMaxPlacementsPerWorkload = 65536;

/// Every unbounded counter in the runtime is one of these. A resource is either
/// accepted under the configured bound or rejected with BoundExceeded; it is never
/// silently truncated and never allocated before the bound is checked.
struct FO_API Bounds {
  std::size_t max_federations = 64;
  std::size_t max_sites = 4096;
  std::size_t max_clusters = 16384;
  std::size_t max_accelerator_classes = 4096;
  std::size_t max_runtimes = 4096;
  std::size_t max_backends = 4096;
  std::size_t max_artifacts = 1u << 20;
  std::size_t max_workloads = 1u << 20;
  std::size_t max_workload_classes = 4096;
  std::size_t max_capability_entries = 1u << 20;
  std::size_t max_capacity_records = 1u << 17;
  std::size_t max_placement_history = 1u << 20;
  std::size_t max_migration_history = 1u << 18;
  std::size_t max_portability_records = 1u << 17;
  std::size_t max_publishers = 1024;
  std::size_t max_publishers_per_federation = 256;
  std::size_t max_domains = 8192;
  std::size_t max_policies = 4096;
  std::size_t max_aggregate_findings = 1u << 18;

  std::size_t max_metadata_bytes = kMaxMetadataBytes;
  std::size_t max_frame_bytes = kMaxFrameBytes;
  std::size_t max_query_response_bytes = kMaxQueryResponseBytes;
  std::size_t max_snapshot_bytes = kMaxSnapshotBytes;
  std::size_t max_persistence_bytes = kMaxPersistenceBytes;
  std::size_t max_evidence_per_finding = kDefaultEvidencePerFinding;

  std::size_t max_threads = 64;
  std::size_t max_connections = 256;
  std::size_t max_ingest_queue_depth = 65536;
  std::size_t max_connections_per_publisher = 4;
  std::size_t max_outstanding_requests_per_connection = 64;
  std::size_t max_history_per_federation = 1u << 20;
  std::size_t max_pools_per_cluster = 1024;

  /// Clamp every field into a supported range and reject impossible combinations.
  [[nodiscard]] Status validate() const;
};

namespace bounds {
/// Maximum number of capacity pools a single cluster may publish.
inline constexpr std::size_t kMaxPoolsPerCluster = 1024;
}  // namespace bounds

/// Conservative defaults used when no configuration is supplied.
[[nodiscard]] FO_API const Bounds& default_bounds() noexcept;

/// Human-readable rendering of the effective bounds, used by `fo-cli bounds`.
[[nodiscard]] FO_API std::string render_bounds(const Bounds& bounds);

}  // namespace fo
