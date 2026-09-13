// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/capability.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/model.hpp"

namespace fo {

/// Named compatibility outcomes. There is deliberately no numeric score: a score
/// cannot be audited, cannot name the failing requirement, and invites consumers to
/// treat 0.81 as "probably fine". Each outcome states what must happen for the target
/// to become eligible.
enum class CompatibilityOutcome : std::uint8_t {
  Compatible = 0,
  CompatibleWithFallback = 1,
  CompatibleAfterReconfiguration = 2,
  CompatibleAfterRebuild = 3,
  CapabilityMissing = 4,
  IncompatibleMemory = 5,
  IncompatibleTopology = 6,
  IncompatibleIsolation = 7,
  IncompatibleArtifact = 8,
  IncompatibleAbi = 9,
  IncompatibleDriver = 10,
  IncompatibleRuntime = 11,
  IncompatibleArchitecture = 12,
  StaleEvidence = 13,
  UnknownCompatibility = 14,
  Unsupported = 15,
};

[[nodiscard]] FO_API std::string_view to_string(CompatibilityOutcome outcome) noexcept;
[[nodiscard]] FO_API bool parse_compatibility_outcome(std::string_view text,
                                                      CompatibilityOutcome& out) noexcept;
/// Severity rank used to combine per-check outcomes. Larger is weaker.
[[nodiscard]] FO_API int compatibility_severity(CompatibilityOutcome outcome) noexcept;
/// The weaker of two outcomes under the severity ordering.
[[nodiscard]] FO_API CompatibilityOutcome weakest(CompatibilityOutcome a,
                                                  CompatibilityOutcome b) noexcept;
/// True when the outcome permits placement on the target as-is or with adaptation.
[[nodiscard]] FO_API bool is_compatible_family(CompatibilityOutcome outcome) noexcept;
/// True when the outcome says the target can never serve this workload generation.
[[nodiscard]] FO_API bool is_hard_incompatible(CompatibilityOutcome outcome) noexcept;
/// True when the outcome is an inability to decide rather than a decision.
[[nodiscard]] FO_API bool is_undecided(CompatibilityOutcome outcome) noexcept;

/// Which relationship a compatibility check evaluated.
enum class CompatibilitySubject : std::uint8_t {
  WorkloadAccelerator = 0,
  ArtifactArchitecture,
  ArtifactFormatSupport,
  ArtifactRuntime,
  ArtifactDriver,
  RuntimeDriver,
  RuntimeOperatingSystem,
  RuntimeAbi,
  CompilerAbi,
  KernelArchitecture,
  ModelFormatBackend,
  AdapterModelGeneration,
  CollectiveTopology,
  PartitionRequirement,
  IsolationRequirement,
  MemoryRequirement,
  PrecisionRequirement,
  CapabilityRequirement,
  PortabilityClassDestination,
  PolicyAdmissibility,
  ClusterCurrentness,
};

[[nodiscard]] FO_API std::string_view to_string(CompatibilitySubject subject) noexcept;

/// One evaluated relationship.
struct FO_API CompatibilityCheck {
  CompatibilitySubject subject = CompatibilitySubject::WorkloadAccelerator;
  std::string requirement;
  std::string observed;
  CompatibilityOutcome outcome = CompatibilityOutcome::UnknownCompatibility;
  ReasonBasis basis = ReasonBasis::Derived;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  std::string detail;

  friend bool operator<(const CompatibilityCheck& a, const CompatibilityCheck& b) noexcept;
};

/// The full compatibility assessment of one workload generation against one target.
struct FO_API CompatibilityAssessment {
  CompatibilityOutcome overall = CompatibilityOutcome::UnknownCompatibility;
  ClusterId target_cluster;
  ClusterGeneration target_cluster_generation;
  ClusterEpoch target_cluster_epoch;
  AcceleratorClassId target_accelerator_class;
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  ArtifactId artifact;
  ArtifactGeneration artifact_generation;
  RuntimeId runtime;
  RuntimeGeneration runtime_generation;
  CompatibilityGeneration compatibility_generation;
  Currentness target_currentness = Currentness::Unknown;
  /// Policy admissibility is tracked separately from technical compatibility on
  /// purpose. A target can be perfectly compatible and still be inadmissible, and the
  /// remedy for each is owned by a different system. It never contributes to `overall`.
  Tri policy_admissible = Tri::Unknown;
  std::string policy_detail;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  std::vector<CompatibilityCheck> checks;
  EvidenceList evidence;

  /// Roles the target could play. Never a boolean: callers must read the role.
  [[nodiscard]] bool admits_placement() const noexcept;
  [[nodiscard]] bool requires_adaptation() const noexcept;
  /// The single most severe failing check, or nullptr when none failed.
  [[nodiscard]] const CompatibilityCheck* primary_failure() const noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Everything the assessor is allowed to look at. Passing explicit inputs keeps the
/// assessment pure and reproducible: no hidden catalog access, no ambient state.
struct FO_API CompatibilityInputs {
  const WorkloadRecord* workload = nullptr;
  const ArtifactRecord* artifact = nullptr;
  const ClusterRecord* cluster = nullptr;
  const AcceleratorClassRecord* accelerator_class = nullptr;
  const RuntimeRecord* cluster_runtime = nullptr;
  const PolicyRecord* policy = nullptr;
  /// Effective capability set of the target: union of cluster capabilities, the
  /// accelerator class capabilities, and the runtime capabilities, each kept distinct
  /// in the evidence trail.
  const CapabilitySet* effective_capabilities = nullptr;
  /// Published capability requirements of the artifact.
  const CapabilitySet* artifact_capabilities = nullptr;
  CompatibilityGeneration compatibility_generation;
  Currentness target_currentness = Currentness::Unknown;
};

/// Evaluate every compatibility relationship in a fixed, documented order.
[[nodiscard]] FO_API CompatibilityAssessment assess_compatibility(const CompatibilityInputs& inputs);

/// Compare a runtime's driver/version fields against a requirement pair.
[[nodiscard]] FO_API CompatibilityOutcome compare_runtime_versions(std::string_view required_runtime_abi,
                                                                   std::string_view observed_runtime_abi,
                                                                   std::string_view required_driver_abi,
                                                                   std::string_view observed_driver_abi) noexcept;

}  // namespace fo
