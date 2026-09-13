// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/capacity.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"

namespace fo {

/// Liveness and authority of one publisher as of a snapshot.
struct FO_API PublisherStatus {
  PublisherId id;
  BootGeneration boot;
  CoordinatorEpoch coordinator_epoch;
  FederationId federation;
  Sequence watermark;
  TimestampNanos registered_at = 0;
  TimestampNanos last_publication_at = 0;
  std::uint64_t publications_accepted = 0;
  std::uint64_t publications_rejected = 0;
  std::uint64_t duplicates_suppressed = 0;
  std::uint64_t sequence_gaps = 0;
  bool live = false;
  bool fenced = false;
  /// Reason the boot identity lost authority, when it did.
  std::string fenced_reason;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
};

/// Freshness census of a snapshot. A snapshot is never called "current federation
/// state" without these counters being visible.
struct FO_API SnapshotHealth {
  std::size_t clusters_total = 0;
  std::size_t clusters_current = 0;
  std::size_t clusters_stale = 0;
  std::size_t clusters_revalidation_required = 0;
  std::size_t clusters_retired = 0;
  std::size_t clusters_unknown = 0;
  std::size_t sites_total = 0;
  std::size_t sites_current = 0;
  std::size_t publishers_total = 0;
  std::size_t publishers_live = 0;
  std::size_t publishers_fenced = 0;
  bool revalidation_pending = false;
  /// True when any dynamic record is not Current. Consumers must render this.
  bool degraded = true;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// An immutable federation observation.
///
/// Snapshots are value-immutable: the runtime hands out shared_ptr<const
/// FederationSnapshot> and never mutates a published snapshot in place. Mutating the
/// live model produces a new snapshot generation, so a reader can hold a snapshot
/// while publications continue without observing torn state.
struct FO_API FederationSnapshot {
  FederationId federation;
  FederationGeneration generation;
  CoordinatorEpoch coordinator_epoch;
  SnapshotGeneration snapshot_generation;
  TimestampNanos created_at = 0;

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
  std::vector<PublisherStatus> publishers;

  SnapshotHealth health;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  /// True when the snapshot was truncated by a bound. A truncated snapshot states so
  /// instead of silently presenting a partial federation as complete.
  bool truncated = false;
  std::size_t records_total = 0;
  std::size_t records_included = 0;

  [[nodiscard]] const FederationRecord* find_federation(const FederationId& id) const;
  [[nodiscard]] const ClusterRecord* find_cluster(const ClusterId& id) const;
  [[nodiscard]] const SiteRecord* find_site(const SiteId& id) const;
  [[nodiscard]] const AcceleratorClassRecord* find_accelerator_class(const AcceleratorClassId& id) const;
  [[nodiscard]] const RuntimeRecord* find_runtime(const RuntimeId& id) const;
  [[nodiscard]] const ArtifactRecord* find_artifact(const ArtifactId& id) const;
  [[nodiscard]] const WorkloadRecord* find_workload(const WorkloadId& id) const;
  [[nodiscard]] const WorkloadClassRecord* find_workload_class(const WorkloadClassId& id) const;
  [[nodiscard]] const PolicyRecord* find_policy(const PolicyId& id) const;
  [[nodiscard]] const PlacementRecord* find_placement(const PlacementId& id) const;
  [[nodiscard]] const MigrationRecord* find_migration(const MigrationId& id) const;
  [[nodiscard]] const DomainRecord* find_domain(const DomainId& id) const;

  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Immutable handle to a snapshot. The runtime never hands out a mutable reference.
using SnapshotHandle = std::shared_ptr<const FederationSnapshot>;

// ---------------------------------------------------------------------------
// Analyses over an immutable snapshot. These are pure functions: the same snapshot
// always yields the same report, byte for byte, which is what the determinism proofs
// assert. Only evidence whose currentness is Current participates unless the request
// explicitly opts into stale evidence.
// ---------------------------------------------------------------------------

[[nodiscard]] FO_API Result<StrandedCapacityReport> analyze_stranded_capacity(
    const FederationSnapshot& snapshot, const StrandedCapacityRequest& request);

[[nodiscard]] FO_API Result<FragmentationFinding> analyze_fragmentation(
    const FederationSnapshot& snapshot, const FederationId& federation,
    const WorkloadClassId& workload_class, ResourceKind kind);

[[nodiscard]] FO_API Result<MismatchAnalysis> analyze_mismatch(const FederationSnapshot& snapshot,
                                                              const MismatchAnalysisRequest& request);

[[nodiscard]] FO_API Result<DriftReport> analyze_drift(const FederationSnapshot& snapshot,
                                                       const DriftRequest& request);

}  // namespace fo
