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

namespace fo {

/// Structured placement rejection reasons. Distinct causes stay distinct: a cluster
/// that is draining, a cluster whose runtime ABI changed, and a cluster nobody asked
/// about are three different facts.
enum class RejectionReason : std::uint8_t {
  Unknown = 0,
  InsufficientAccelerators = 1,
  InsufficientMemory = 2,
  CapabilityMissing = 3,
  ArtifactIncompatible = 4,
  RuntimeIncompatible = 5,
  DriverIncompatible = 6,
  TopologyIncompatible = 7,
  IsolationIncompatible = 8,
  AbiIncompatible = 9,
  PolicyRejected = 10,
  ClusterDraining = 11,
  ClusterNotReady = 12,
  StaleClusterEvidence = 13,
  CapacityReserved = 14,
  PortabilityNotProven = 15,
  SiteRestriction = 16,
  LocalityConstraint = 17,
  FailureDomainConstraint = 18,
};

inline constexpr std::size_t kRejectionReasonCount = 19;

[[nodiscard]] FO_API std::string_view to_string(RejectionReason reason) noexcept;
[[nodiscard]] FO_API bool parse_rejection_reason(std::string_view text, RejectionReason& out) noexcept;
/// Deterministic precedence (most specific first) used when a consumer needs a single
/// headline reason while the record itself keeps every stated reason.
[[nodiscard]] FO_API std::size_t rejection_reason_precedence(RejectionReason reason) noexcept;
/// Map a stranding reason onto the rejection vocabulary where the concepts coincide.
[[nodiscard]] FO_API RejectionReason reject_reason_for_stranding(StrandingReason reason) noexcept;

/// Part of a RejectionReason bitmask.
using RejectionMask = std::uint32_t;
[[nodiscard]] FO_API constexpr RejectionMask rejection_bit(RejectionReason reason) noexcept {
  return static_cast<RejectionMask>(1u) << static_cast<unsigned>(reason);
}

/// Status of one candidate cluster in a placement decision.
enum class CandidateStatus : std::uint8_t {
  Selected = 0,
  Rejected = 1,
  NotConsidered = 2,  ///< The source stated the candidate was filtered before evaluation.
  Unattributed = 3,   ///< Present in the candidate set, no outcome observed.
};

[[nodiscard]] FO_API std::string_view to_string(CandidateStatus status) noexcept;

/// One observed candidate row.
struct FO_API CandidateObservation {
  ClusterId cluster;
  ClusterGeneration cluster_generation;
  ClusterEpoch cluster_epoch;
  AcceleratorClassId accelerator_class;
  CandidateStatus status = CandidateStatus::Unattributed;
  RejectionMask reasons = 0;
  /// Stated by the source rather than computed by the runtime.
  ReasonBasis basis = ReasonBasis::Unattributed;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  std::string detail;
  EvidenceList evidence;

  [[nodiscard]] bool has_reason(RejectionReason reason) const noexcept {
    return (reasons & rejection_bit(reason)) != 0;
  }
  [[nodiscard]] std::vector<RejectionReason> reason_list() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
  friend bool operator<(const CandidateObservation& a, const CandidateObservation& b) noexcept;
};

/// How complete the observed candidate set is. Placement attribution must never claim
/// more coverage than the upstream scheduler actually exposed.
enum class CandidateSetCompleteness : std::uint8_t {
  Unknown = 0,
  SelectedOnly = 1,  ///< Only the chosen target is known; rejection attribution unavailable.
  Partial = 2,       ///< Some rejected candidates were exposed.
  Complete = 3,      ///< The source stated the full evaluated candidate set.
};

[[nodiscard]] FO_API std::string_view to_string(CandidateSetCompleteness completeness) noexcept;
[[nodiscard]] FO_API bool allows_rejection_attribution(CandidateSetCompleteness c) noexcept;

/// An observed placement decision.
struct FO_API PlacementRecord {
  PlacementId id;
  PlacementGeneration generation;
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  FederationId federation;
  FederationGeneration federation_generation;
  WorkloadClassId workload_class;

  ClusterId selected;
  ClusterGeneration selected_generation;
  ClusterEpoch selected_epoch;
  AcceleratorClassId selected_accelerator_class;
  std::uint32_t selected_accelerator_count = 0;
  std::uint64_t selected_memory_bytes = 0;

  std::vector<CandidateObservation> candidates;
  CandidateSetCompleteness candidate_completeness = CandidateSetCompleteness::Unknown;

  PolicyGeneration policy_generation;
  CapacityGeneration capacity_generation;
  TopologyGeneration topology_generation;
  CompatibilityGeneration compatibility_generation;
  RuntimeGeneration runtime_generation;

  bool capacity_available = false;
  Tri compatibility_constrained = Tri::Unknown;
  Tri topology_constrained = Tri::Unknown;
  Tri policy_constrained = Tri::Unknown;
  Tri capability_constrained = Tri::Unknown;
  Tri affinity_constrained = Tri::Unknown;
  bool fallback_required = false;
  std::string fallback_detail;

  bool cost_known = false;
  double cost_estimate = 0.0;
  std::string slo_class;

  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] const CandidateObservation* find_candidate(const ClusterId& cluster) const;
  [[nodiscard]] std::size_t rejected_count() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Aspect of a placement that an attribution finding describes.
enum class AttributionAspect : std::uint8_t {
  Selection = 0,
  Eligibility,
  Capability,
  Compatibility,
  Capacity,
  Topology,
  Policy,
  Affinity,
  Fallback,
  Rejection,
  CandidateCoverage,
  Currentness,
};

[[nodiscard]] FO_API std::string_view to_string(AttributionAspect aspect) noexcept;

/// One statement in a placement explanation, carrying its own basis and evidence.
struct FO_API AttributionFinding {
  AttributionAspect aspect = AttributionAspect::Selection;
  std::string subject;
  std::string statement;
  ReasonBasis basis = ReasonBasis::Derived;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  friend bool operator<(const AttributionFinding& a, const AttributionFinding& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A deterministic, provenance-preserving explanation of one placement.
struct FO_API PlacementExplanation {
  PlacementId placement;
  PlacementGeneration placement_generation;
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  ClusterId selected;
  CandidateSetCompleteness candidate_completeness = CandidateSetCompleteness::Unknown;
  std::vector<AttributionFinding> findings;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  Currentness currentness = Currentness::Unknown;
  SnapshotGeneration snapshot_generation;

  /// Stable fingerprint of the explanation content. Identical inputs must produce an
  /// identical digest; this is what the determinism property test asserts.
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
  [[nodiscard]] bool has_rejection_attribution() const noexcept;
};

/// Explanation of why one candidate did not receive the placement.
struct FO_API RejectionExplanation {
  PlacementId placement;
  PlacementGeneration placement_generation;
  ClusterId candidate;
  ClusterGeneration candidate_generation;
  CandidateStatus status = CandidateStatus::Unattributed;
  std::vector<RejectionReason> reasons;
  ReasonBasis basis = ReasonBasis::Unattributed;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  /// False when the upstream scheduler did not expose the candidate set. In that case
  /// the runtime reports "rejection attribution unavailable" rather than guessing.
  bool attribution_available = false;
  std::string summary;
  EvidenceList evidence;

  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
};

/// Everything needed to explain a placement, supplied by the caller so that the
/// explainer is a pure function over immutable inputs.
struct FO_API PlacementExplanationInputs {
  const PlacementRecord* placement = nullptr;
  /// Compatibility assessments keyed by cluster, for the candidates that were assessed.
  std::vector<CompatibilityAssessment> assessments;
  SnapshotGeneration snapshot_generation;

  [[nodiscard]] const CompatibilityAssessment* find(const ClusterId& cluster) const;
};

/// Build the explanation. Never asserts a reason the evidence does not support.
[[nodiscard]] FO_API PlacementExplanation explain_placement(const PlacementExplanationInputs& inputs);

/// Build the rejection explanation for one candidate. When the source did not expose
/// the candidate set, `attribution_available` is false and the summary says so rather
/// than inventing a reason.
[[nodiscard]] FO_API RejectionExplanation build_rejection_explanation(const PlacementRecord& placement,
                                                                     const ClusterId& candidate);

}  // namespace fo
