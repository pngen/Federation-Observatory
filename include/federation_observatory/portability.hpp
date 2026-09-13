// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/model.hpp"

namespace fo {

/// Independent portability dimensions. Portability is never one boolean: an artifact
/// can be binary-portable and state-incompatible at the same time, and that difference
/// determines whether a migration needs a conversion step or a rollback.
enum class PortabilityDimension : std::uint8_t {
  Binary = 0,
  Artifact = 1,
  Model = 2,
  Checkpoint = 3,
  RuntimeApi = 4,
  Kernel = 5,
  DataFormat = 6,
  State = 7,
  NumericalSemantic = 8,
  Performance = 9,
  Operational = 10,
};

inline constexpr std::size_t kPortabilityDimensionCount = 11;

[[nodiscard]] FO_API std::string_view to_string(PortabilityDimension dimension) noexcept;
[[nodiscard]] FO_API bool parse_portability_dimension(std::string_view text,
                                                      PortabilityDimension& out) noexcept;

/// Structured portability outcomes.
enum class PortabilityOutcome : std::uint8_t {
  PortableDirect = 0,
  PortableWithFallback = 1,
  PortableWithRebuild = 2,
  PortableWithRecompile = 3,
  PortableWithConversion = 4,
  PortableWithStateTranslation = 5,
  NotPortableTopology = 6,
  NotPortablePolicy = 7,
  NotPortableArtifact = 8,
  NotPortableRuntime = 9,
  NotPortableState = 10,
  NotPortableArchitecture = 11,
  Unknown = 12,
  Unsupported = 13,
};

inline constexpr std::size_t kPortabilityOutcomeCount = 14;

[[nodiscard]] FO_API std::string_view to_string(PortabilityOutcome outcome) noexcept;
[[nodiscard]] FO_API bool parse_portability_outcome(std::string_view text,
                                                    PortabilityOutcome& out) noexcept;
[[nodiscard]] FO_API int portability_severity(PortabilityOutcome outcome) noexcept;
[[nodiscard]] FO_API PortabilityOutcome weakest(PortabilityOutcome a, PortabilityOutcome b) noexcept;
/// True when the outcome requires a build or conversion step before the move is legal.
[[nodiscard]] FO_API bool requires_adaptation(PortabilityOutcome outcome) noexcept;
/// True when the outcome is a technical incompatibility.
[[nodiscard]] FO_API bool is_technical_failure(PortabilityOutcome outcome) noexcept;
/// True when the outcome is a policy or administrative restriction. Kept strictly
/// separate from technical failure: they have different owners and different remedies.
[[nodiscard]] FO_API bool is_policy_failure(PortabilityOutcome outcome) noexcept;
/// True when no evidence supports a decision.
[[nodiscard]] FO_API bool is_undecided(PortabilityOutcome outcome) noexcept;

/// Result for one dimension.
struct FO_API PortabilityDimensionResult {
  PortabilityDimension dimension = PortabilityDimension::Binary;
  PortabilityOutcome outcome = PortabilityOutcome::Unknown;
  /// False when nothing in the workload or artifact constrains this dimension, so its
  /// outcome is reported but does not decide the overall verdict. Performance portability
  /// is never measured by this runtime and therefore never constrains a verdict.
  bool constrains_overall = true;
  std::string detail;
  ReasonBasis basis = ReasonBasis::Derived;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  friend bool operator<(const PortabilityDimensionResult& a,
                        const PortabilityDimensionResult& b) noexcept;
};

/// Full portability assessment for one source/destination pair of one workload
/// generation. Every dimension is reported; absent evidence yields Unknown, never
/// PortableDirect.
struct FO_API PortabilityAssessment {
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  ArtifactId artifact;
  ArtifactGeneration artifact_generation;
  WorkloadClassId workload_class;

  ClusterId source;
  ClusterGeneration source_generation;
  ClusterEpoch source_epoch;
  ClusterId destination;
  ClusterGeneration destination_generation;
  ClusterEpoch destination_epoch;

  RuntimeId source_runtime;
  RuntimeGeneration source_runtime_generation;
  RuntimeId destination_runtime;
  RuntimeGeneration destination_runtime_generation;

  FederationId federation;
  FederationGeneration federation_generation;
  CompatibilityGeneration compatibility_generation;
  PolicyGeneration policy_generation;

  std::vector<PortabilityDimensionResult> dimensions;
  PortabilityOutcome overall = PortabilityOutcome::Unknown;

  bool requires_rebuild = false;
  bool requires_recompile = false;
  bool requires_conversion = false;
  bool requires_state_translation = false;
  bool requires_fallback = false;
  bool technically_blocked = false;
  bool policy_blocked = false;

  Currentness currentness = Currentness::Unknown;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] const PortabilityDimensionResult* find(PortabilityDimension dimension) const;
  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Everything the portability assessor is allowed to read. As with compatibility, the
/// assessor is a pure function so that two evaluations of the same evidence cannot
/// disagree.
///
/// Evaluation rules that matter for honesty:
///  * a dimension with no evidence yields Unknown, never PortableDirect;
///  * performance portability is never asserted without measured evidence;
///  * a destination that is not operationally ready is an operational failure, which is
///    reported separately from a technical or a policy failure.
struct FO_API PortabilityInputs {
  const WorkloadRecord* workload = nullptr;
  const ArtifactRecord* artifact = nullptr;
  const ClusterRecord* source_cluster = nullptr;
  const AcceleratorClassRecord* source_accelerator_class = nullptr;
  const RuntimeRecord* source_runtime = nullptr;
  const ClusterRecord* destination_cluster = nullptr;
  const AcceleratorClassRecord* destination_accelerator_class = nullptr;
  const RuntimeRecord* destination_runtime = nullptr;
  const PolicyRecord* policy = nullptr;
  /// Capability set of the destination: union of destination cluster and accelerator
  /// class capabilities.
  const CapabilitySet* destination_capabilities = nullptr;
  FederationId federation;
  FederationGeneration federation_generation;
  CompatibilityGeneration compatibility_generation;
  PolicyGeneration policy_generation;
};

[[nodiscard]] FO_API PortabilityAssessment assess_portability(const PortabilityInputs& inputs);

}  // namespace fo
