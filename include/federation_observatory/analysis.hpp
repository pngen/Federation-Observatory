// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/capacity.hpp"
#include "federation_observatory/compatibility.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"

namespace fo {

/// Half-open observation window over local observation timestamps.
struct FO_API TimeWindow {
  TimestampNanos from = 0;
  TimestampNanos to = 0;
  [[nodiscard]] bool unbounded() const noexcept { return from == 0 && to == 0; }
  [[nodiscard]] bool contains(TimestampNanos t) const noexcept {
    if (from != 0 && t < from) return false;
    if (to != 0 && t >= to) return false;
    return true;
  }
  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Stranded capacity
// ---------------------------------------------------------------------------

/// Per-scope stranded-capacity finding for one resource kind and workload class.
struct FO_API StrandedCapacityFinding {
  FederationId federation;
  FederationGeneration federation_generation;
  WorkloadClassId workload_class;
  ClusterId cluster;
  ClusterGeneration cluster_generation;
  ClusterEpoch cluster_epoch;
  SiteId site;
  AcceleratorClassId accelerator_class;
  ResourcePoolId pool;
  ResourceKind kind = ResourceKind::Accelerator;

  CapacityGeneration capacity_generation;
  AcceleratorCapabilityGeneration capability_generation;
  TopologyGeneration topology_generation;
  CompatibilityGeneration compatibility_generation;

  CapacityLedger ledger;
  EligibilityBreakdown breakdown;
  Currentness currentness = Currentness::Unknown;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  friend bool operator<(const StrandedCapacityFinding& a, const StrandedCapacityFinding& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Selection criteria for a stranded-capacity analysis. An empty identifier means
/// "do not filter on this dimension".
struct FO_API StrandedCapacityRequest {
  FederationId federation;
  WorkloadClassId workload_class;
  ResourceKind kind = ResourceKind::Accelerator;
  ClusterId cluster;
  AcceleratorClassId accelerator_class;
  bool include_stale = false;
};

/// Report for one workload class and resource kind.
struct FO_API StrandedCapacityReport {
  FederationId federation;
  FederationGeneration federation_generation;
  CoordinatorEpoch coordinator_epoch;
  SnapshotGeneration snapshot_generation;
  WorkloadClassId workload_class;
  ResourceKind kind = ResourceKind::Accelerator;

  CapacitySummary summary;
  std::vector<StrandedCapacityFinding> findings;
  /// Clusters whose evidence is stale or missing, so the report is explicitly partial.
  std::vector<ClusterId> excluded_stale_clusters;
  std::vector<ClusterId> excluded_unknown_clusters;

  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  [[nodiscard]] bool closes() const noexcept;
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Federation fragmentation
// ---------------------------------------------------------------------------

/// Why a required co-dependent group cannot be assembled.
enum class FragmentationClass : std::uint8_t {
  None = 0,
  CapacityShortage = 1,        ///< Aggregate usable capacity is genuinely insufficient.
  PhysicalFragmentation = 2,   ///< Enough devices exist, none in one legal domain.
  CompatibilityFragmentation = 3,  ///< Devices exist but differ in capability generation.
  PolicyFragmentation = 4,     ///< Legal devices exist but policy separates them.
  TopologyFragmentation = 5,   ///< Interconnect/NUMA constraints split the group.
  SiteFragmentation = 6,       ///< Group would span sites, cross-site not permitted.
  Mixed = 7,
  Unknown = 8,
};

[[nodiscard]] FO_API std::string_view to_string(FragmentationClass cls) noexcept;
[[nodiscard]] FO_API bool parse_fragmentation_class(std::string_view text,
                                                    FragmentationClass& out) noexcept;

/// Usable capacity of one placement domain.
struct FO_API DomainCapacity {
  DomainId domain;
  DomainKind kind = DomainKind::Unknown;
  SiteId site;
  std::uint64_t nominal = 0;
  std::uint64_t usable = 0;
  std::uint64_t stranded = 0;
  std::uint64_t unknown = 0;
  std::vector<ClusterId> clusters;

  friend bool operator<(const DomainCapacity& a, const DomainCapacity& b) noexcept;
};

/// Federation-level fragmentation finding for one workload class and required group.
struct FO_API FragmentationFinding {
  FederationId federation;
  FederationGeneration federation_generation;
  WorkloadClassId workload_class;
  ResourceKind kind = ResourceKind::Accelerator;
  DomainKind required_domain_kind = DomainKind::Unknown;

  std::uint64_t required_per_group = 0;
  std::uint64_t aggregate_nominal = 0;
  std::uint64_t aggregate_usable = 0;
  std::uint64_t aggregate_stranded = 0;
  std::uint64_t aggregate_unknown = 0;
  std::uint64_t largest_legal_group = 0;
  DomainId largest_domain;
  std::uint64_t legal_domain_count = 0;

  FragmentationClass classification = FragmentationClass::Unknown;
  std::vector<DomainCapacity> domains;
  /// Amounts stranded per reason, summed over the federation.
  ReasonTally stranded_by_reason;

  CapacityGeneration capacity_generation;
  AcceleratorCapabilityGeneration capability_generation;
  TopologyGeneration topology_generation;
  Currentness currentness = Currentness::Unknown;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

// ---------------------------------------------------------------------------
// Capability mismatch analysis
// ---------------------------------------------------------------------------

/// One row of an aggregate mismatch table.
struct FO_API MismatchRow {
  std::string subject;
  std::uint64_t population = 0;
  std::uint64_t satisfied = 0;
  std::uint64_t missing = 0;
  std::uint64_t unknown = 0;
  Precision precision = Precision::Unknown;

  friend bool operator<(const MismatchRow& a, const MismatchRow& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Aggregate mismatch analysis: what is missing, where, and how much of the fleet is
/// affected. Population counts are stated explicitly so that no percentage is read
/// without its denominator.
struct FO_API MismatchAnalysis {
  FederationId federation;
  FederationGeneration federation_generation;
  SnapshotGeneration snapshot_generation;
  TimeWindow window;

  std::vector<MismatchRow> by_capability_key;
  std::vector<MismatchRow> by_accelerator_class;
  std::vector<MismatchRow> by_cluster;
  std::vector<MismatchRow> by_site;
  std::vector<MismatchRow> by_runtime;
  std::vector<MismatchRow> by_workload_class;
  std::vector<MismatchRow> by_artifact;

  /// Placements in the window rejected per structured reason.
  std::vector<MismatchRow> placement_rejections;
  /// Capacity stranded per reason, in the requested resource kind.
  std::vector<MismatchRow> capacity_stranded_by_reason;
  /// Migrations in the window requiring each kind of adaptation.
  std::vector<MismatchRow> migration_adaptations;
  /// Portability outcomes observed in the window.
  std::vector<MismatchRow> portability_outcomes;

  std::uint64_t placements_observed = 0;
  std::uint64_t migrations_observed = 0;
  std::uint64_t portability_records_observed = 0;

  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Drift
// ---------------------------------------------------------------------------

/// Kinds of drift between an intended federation description and observed state, and
/// kinds of drift in placement behavior between two observation windows.
enum class DriftKind : std::uint8_t {
  ClusterMissing = 0,
  UnexpectedCluster = 1,
  SiteMissing = 2,
  UnexpectedSite = 3,
  CapabilityMismatch = 4,
  RuntimeVersionDivergence = 5,
  StaleCluster = 6,
  PolicyGenerationMismatch = 7,
  ArtifactCompatibilityDrift = 8,
  CapacityReportDivergence = 9,
  PlacementTargetChanged = 10,
  PlacementRejectionIntroduced = 11,
  PlacementRejectionCleared = 12,
  PlacementFallbackIntroduced = 13,
  PlacementCompatibilityChanged = 14,
};

[[nodiscard]] FO_API std::string_view to_string(DriftKind kind) noexcept;
[[nodiscard]] FO_API bool parse_drift_kind(std::string_view text, DriftKind& out) noexcept;
/// True when the drift kind describes a change in observed placement behavior.
[[nodiscard]] FO_API bool is_behavioral_drift(DriftKind kind) noexcept;
/// True when the drift kind is only meaningful against a supplied intended state.
[[nodiscard]] FO_API bool requires_intended_state(DriftKind kind) noexcept;

/// An externally supplied description of what the federation is *supposed* to be.
/// The runtime never invents one: with no intended state, intended-state drift is
/// reported as not evaluated rather than as a clean bill of health.
struct FO_API IntendedFederationState {
  FederationId federation;
  FederationGeneration generation;
  std::vector<SiteId> expected_sites;
  std::vector<ClusterId> expected_clusters;
  struct ExpectedCapability {
    ClusterId cluster;
    CapabilityRef key;
    CapabilityValue value;
    CapabilityComparator comparator = CapabilityComparator::Present;
  };
  std::vector<ExpectedCapability> expected_capabilities;
  std::vector<std::pair<ClusterId, std::string>> expected_runtime_versions;
  std::vector<std::pair<ClusterId, CapacityLedger>> expected_capacity;
  PolicyGeneration expected_policy_generation;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
};

/// Selection criteria for aggregate mismatch analysis.
struct FO_API MismatchAnalysisRequest {
  FederationId federation;
  /// Restrict the capability rows to one key. Unknown key means "all keys observed".
  CapabilityRef capability_key;
  TimeWindow window;
  ResourceKind kind = ResourceKind::Accelerator;
  /// Include records whose evidence is not Current. Default false: an aggregate built
  /// from stale evidence must be requested explicitly.
  bool include_stale = false;
};

/// Selection criteria for drift analysis.
struct FO_API DriftRequest {
  FederationId federation;
  /// Intended state to compare against. Null means intended-state drift is not
  /// evaluated, and the report says so instead of reporting no drift.
  const IntendedFederationState* intended = nullptr;
  bool behavior_window_supplied = false;
  TimeWindow before_window;
  TimeWindow after_window;
};

/// A single drift observation with before/after evidence.
struct FO_API DriftFinding {
  DriftKind kind = DriftKind::ClusterMissing;
  std::string subject;
  std::string intended;
  std::string observed;
  ReasonBasis basis = ReasonBasis::Derived;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  friend bool operator<(const DriftFinding& a, const DriftFinding& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

struct FO_API DriftReport {
  FederationId federation;
  FederationGeneration federation_generation;
  SnapshotGeneration snapshot_generation;
  bool intended_state_supplied = false;
  bool behavior_window_supplied = false;
  TimeWindow before_window;
  TimeWindow after_window;
  std::vector<DriftFinding> findings;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  [[nodiscard]] std::uint64_t count_of(DriftKind kind) const noexcept;
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
};

}  // namespace fo
