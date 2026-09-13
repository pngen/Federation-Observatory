// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Explicit, explainable compatibility. Every relationship is evaluated in a fixed
// order and yields a named outcome; there is no opaque score anywhere in this file.

#include <algorithm>
#include <string>
#include <vector>

#include "federation_observatory/compatibility.hpp"
#include "federation_observatory/explanation.hpp"

namespace fo {
namespace {

bool is_code_bearing(ArtifactKind kind) {
  switch (kind) {
    case ArtifactKind::ExecutableBinary:
    case ArtifactKind::SharedLibrary:
    case ArtifactKind::DeviceImage:
    case ArtifactKind::ContainerImage:
      return true;
    default:
      return false;
  }
}

CompatibilityOutcome outcome_for_failed_requirement(CapabilityKey key) {
  switch (key) {
    case CapabilityKey::MemoryBytes:
    case CapabilityKey::MemoryBandwidthGbps:
      return CompatibilityOutcome::IncompatibleMemory;
    case CapabilityKey::PartitionSupport:
    case CapabilityKey::PartitionModes:
    case CapabilityKey::PartitionGeometry:
    case CapabilityKey::PartitionMaxInstances:
      return CompatibilityOutcome::IncompatibleTopology;
    case CapabilityKey::SecurityIsolation:
      return CompatibilityOutcome::IncompatibleIsolation;
    case CapabilityKey::ArtifactFormat:
    case CapabilityKey::ArtifactFormats:
    case CapabilityKey::QuantizationSupport:
    case CapabilityKey::QuantizationFormats:
    case CapabilityKey::SparseExecutionSupport:
    case CapabilityKey::InferenceServingBackend:
      return CompatibilityOutcome::IncompatibleArtifact;
    case CapabilityKey::RuntimeApi:
    case CapabilityKey::RuntimeVersion:
    case CapabilityKey::RuntimeGeneration:
      return CompatibilityOutcome::IncompatibleRuntime;
    case CapabilityKey::RuntimeAbi:
    case CapabilityKey::CompilerTarget:
    case CapabilityKey::CompilerVersion:
      return CompatibilityOutcome::IncompatibleAbi;
    case CapabilityKey::DriverApi:
    case CapabilityKey::DriverVersion:
    case CapabilityKey::DriverAbi:
    case CapabilityKey::DriverGeneration:
      return CompatibilityOutcome::IncompatibleDriver;
    case CapabilityKey::KernelFormat:
    case CapabilityKey::KernelArchitectures:
      return CompatibilityOutcome::IncompatibleArtifact;
    case CapabilityKey::AcceleratorArchitecture:
      return CompatibilityOutcome::IncompatibleArchitecture;
    default:
      return CompatibilityOutcome::CapabilityMissing;
  }
}

struct Builder {
  CompatibilityAssessment assessment;
  const CompatibilityInputs* inputs = nullptr;

  void add(CompatibilitySubject subject, std::string requirement, std::string observed,
           CompatibilityOutcome outcome, std::string detail) {
    CompatibilityCheck check;
    check.subject = subject;
    check.requirement = std::move(requirement);
    check.observed = std::move(observed);
    check.outcome = outcome;
    check.basis = ReasonBasis::Derived;
    check.precision = Precision::Derived;
    check.evidence_class = inputs->effective_capabilities != nullptr
                               ? inputs->effective_capabilities->combined_class()
                               : EvidenceClass::Unknown;
    if (check.evidence_class == EvidenceClass::Unknown && inputs->cluster != nullptr) {
      check.evidence_class = inputs->cluster->stamp.evidence_class;
    }
    check.detail = std::move(detail);
    assessment.checks.push_back(std::move(check));
    if (!is_undecided(outcome) || is_undecided(assessment.overall)) {
      assessment.overall = weakest(assessment.overall, outcome);
    } else {
      assessment.overall = weakest(assessment.overall, outcome);
    }
  }

  void add_capability_requirement(const CapabilityRequirement& requirement, bool from_artifact) {
    if (inputs->effective_capabilities == nullptr) {
      add(CapabilitySubjectFor(requirement), requirement.to_string(), "UNKNOWN",
          CompatibilityOutcome::UnknownCompatibility,
          "no capability evidence is available for the target");
      return;
    }
    const CapabilityCheck check = evaluate_requirement(requirement, *inputs->effective_capabilities);
    CompatibilityOutcome outcome = CompatibilityOutcome::Compatible;
    if (check.satisfied == Tri::No) {
      outcome = outcome_for_failed_requirement(requirement.key.key);
      if (requirement.optional) {
        outcome = CompatibilityOutcome::CompatibleWithFallback;
      }
    } else if (check.satisfied == Tri::Unknown) {
      outcome = CompatibilityOutcome::UnknownCompatibility;
    }
    std::string detail = check.detail;
    if (requirement.optional && check.satisfied == Tri::No) {
      detail += " (optional requirement; recorded as a fallback, not a rejection)";
    }
    if (from_artifact) {
      detail += " [artifact-declared requirement]";
    }
    add(CapabilitySubjectFor(requirement), requirement.to_string(),
        check.observed.known() ? check.observed.to_string() : std::string("UNKNOWN"), outcome,
        std::move(detail));
  }

  static CompatibilitySubject CapabilitySubjectFor(const CapabilityRequirement& requirement) {
    switch (requirement.key.key) {
      case CapabilityKey::MemoryBytes:
      case CapabilityKey::MemoryBandwidthGbps:
        return CompatibilitySubject::MemoryRequirement;
      case CapabilityKey::PartitionSupport:
      case CapabilityKey::PartitionModes:
      case CapabilityKey::PartitionGeometry:
      case CapabilityKey::PartitionMaxInstances:
        return CompatibilitySubject::PartitionRequirement;
      case CapabilityKey::SecurityIsolation:
        return CompatibilitySubject::IsolationRequirement;
      case CapabilityKey::CollectiveSupport:
      case CapabilityKey::CollectiveBackend:
      case CapabilityKey::CollectiveMaxGroupSize:
        return CompatibilitySubject::CollectiveTopology;
      case CapabilityKey::KernelFormat:
      case CapabilityKey::KernelArchitectures:
        return CompatibilitySubject::KernelArchitecture;
      case CapabilityKey::ArtifactFormat:
      case CapabilityKey::ArtifactFormats:
        return CompatibilitySubject::ArtifactFormatSupport;
      case CapabilityKey::RuntimeAbi:
      case CapabilityKey::CompilerTarget:
      case CapabilityKey::CompilerVersion:
        return CompatibilitySubject::CompilerAbi;
      case CapabilityKey::RuntimeApi:
      case CapabilityKey::RuntimeVersion:
      case CapabilityKey::RuntimeGeneration:
        return CompatibilitySubject::ArtifactRuntime;
      case CapabilityKey::DriverApi:
      case CapabilityKey::DriverVersion:
      case CapabilityKey::DriverAbi:
      case CapabilityKey::DriverGeneration:
        return CompatibilitySubject::ArtifactDriver;
      case CapabilityKey::AcceleratorArchitecture:
        return CompatibilitySubject::ArtifactArchitecture;
      default:
        return CompatibilitySubject::CapabilityRequirement;
    }
  }
};

}  // namespace

std::string_view to_string(CompatibilityOutcome outcome) noexcept {
  switch (outcome) {
    case CompatibilityOutcome::Compatible: return "COMPATIBLE";
    case CompatibilityOutcome::CompatibleWithFallback: return "COMPATIBLE_WITH_FALLBACK";
    case CompatibilityOutcome::CompatibleAfterReconfiguration: return "COMPATIBLE_AFTER_RECONFIGURATION";
    case CompatibilityOutcome::CompatibleAfterRebuild: return "COMPATIBLE_AFTER_REBUILD";
    case CompatibilityOutcome::CapabilityMissing: return "CAPABILITY_MISSING";
    case CompatibilityOutcome::IncompatibleMemory: return "INCOMPATIBLE_MEMORY";
    case CompatibilityOutcome::IncompatibleTopology: return "INCOMPATIBLE_TOPOLOGY";
    case CompatibilityOutcome::IncompatibleIsolation: return "INCOMPATIBLE_ISOLATION";
    case CompatibilityOutcome::IncompatibleArtifact: return "INCOMPATIBLE_ARTIFACT";
    case CompatibilityOutcome::IncompatibleAbi: return "INCOMPATIBLE_ABI";
    case CompatibilityOutcome::IncompatibleDriver: return "INCOMPATIBLE_DRIVER";
    case CompatibilityOutcome::IncompatibleRuntime: return "INCOMPATIBLE_RUNTIME";
    case CompatibilityOutcome::IncompatibleArchitecture: return "INCOMPATIBLE_ARCHITECTURE";
    case CompatibilityOutcome::StaleEvidence: return "STALE_EVIDENCE";
    case CompatibilityOutcome::UnknownCompatibility: return "UNKNOWN_COMPATIBILITY";
    case CompatibilityOutcome::Unsupported: return "UNSUPPORTED";
  }
  return "UNKNOWN_COMPATIBILITY";
}

bool parse_compatibility_outcome(std::string_view text, CompatibilityOutcome& out) noexcept {
  for (int i = 0; i <= static_cast<int>(CompatibilityOutcome::Unsupported); ++i) {
    const auto candidate = static_cast<CompatibilityOutcome>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

int compatibility_severity(CompatibilityOutcome outcome) noexcept {
  switch (outcome) {
    case CompatibilityOutcome::Compatible: return 0;
    case CompatibilityOutcome::CompatibleWithFallback: return 1;
    case CompatibilityOutcome::CompatibleAfterReconfiguration: return 2;
    case CompatibilityOutcome::CompatibleAfterRebuild: return 3;
    case CompatibilityOutcome::CapabilityMissing: return 4;
    case CompatibilityOutcome::IncompatibleMemory: return 5;
    case CompatibilityOutcome::IncompatibleTopology: return 6;
    case CompatibilityOutcome::IncompatibleIsolation: return 7;
    case CompatibilityOutcome::IncompatibleArtifact: return 8;
    case CompatibilityOutcome::IncompatibleAbi: return 9;
    case CompatibilityOutcome::IncompatibleDriver: return 10;
    case CompatibilityOutcome::IncompatibleRuntime: return 11;
    case CompatibilityOutcome::IncompatibleArchitecture: return 12;
    case CompatibilityOutcome::StaleEvidence: return 13;
    case CompatibilityOutcome::UnknownCompatibility: return 14;
    case CompatibilityOutcome::Unsupported: return 15;
  }
  return 14;
}

CompatibilityOutcome weakest(CompatibilityOutcome a, CompatibilityOutcome b) noexcept {
  return compatibility_severity(a) >= compatibility_severity(b) ? a : b;
}

bool is_compatible_family(CompatibilityOutcome outcome) noexcept {
  return compatibility_severity(outcome) <= 3;
}

bool is_hard_incompatible(CompatibilityOutcome outcome) noexcept {
  const int severity = compatibility_severity(outcome);
  return severity >= 4 && severity <= 12;
}

bool is_undecided(CompatibilityOutcome outcome) noexcept {
  return compatibility_severity(outcome) >= 13;
}

std::string_view to_string(CompatibilitySubject subject) noexcept {
  switch (subject) {
    case CompatibilitySubject::WorkloadAccelerator: return "WORKLOAD_ACCELERATOR";
    case CompatibilitySubject::ArtifactArchitecture: return "ARTIFACT_ARCHITECTURE";
    case CompatibilitySubject::ArtifactFormatSupport: return "ARTIFACT_FORMAT_SUPPORT";
    case CompatibilitySubject::ArtifactRuntime: return "ARTIFACT_RUNTIME";
    case CompatibilitySubject::ArtifactDriver: return "ARTIFACT_DRIVER";
    case CompatibilitySubject::RuntimeDriver: return "RUNTIME_DRIVER";
    case CompatibilitySubject::RuntimeOperatingSystem: return "RUNTIME_OPERATING_SYSTEM";
    case CompatibilitySubject::RuntimeAbi: return "RUNTIME_ABI";
    case CompatibilitySubject::CompilerAbi: return "COMPILER_ABI";
    case CompatibilitySubject::KernelArchitecture: return "KERNEL_ARCHITECTURE";
    case CompatibilitySubject::ModelFormatBackend: return "MODEL_FORMAT_BACKEND";
    case CompatibilitySubject::AdapterModelGeneration: return "ADAPTER_MODEL_GENERATION";
    case CompatibilitySubject::CollectiveTopology: return "COLLECTIVE_TOPOLOGY";
    case CompatibilitySubject::PartitionRequirement: return "PARTITION_REQUIREMENT";
    case CompatibilitySubject::IsolationRequirement: return "ISOLATION_REQUIREMENT";
    case CompatibilitySubject::MemoryRequirement: return "MEMORY_REQUIREMENT";
    case CompatibilitySubject::PrecisionRequirement: return "PRECISION_REQUIREMENT";
    case CompatibilitySubject::CapabilityRequirement: return "CAPABILITY_REQUIREMENT";
    case CompatibilitySubject::PortabilityClassDestination: return "PORTABILITY_CLASS_DESTINATION";
    case CompatibilitySubject::PolicyAdmissibility: return "POLICY_ADMISSIBILITY";
    case CompatibilitySubject::ClusterCurrentness: return "CLUSTER_CURRENTNESS";
  }
  return "CAPABILITY_REQUIREMENT";
}

bool operator<(const CompatibilityCheck& a, const CompatibilityCheck& b) noexcept {
  if (a.subject != b.subject) return a.subject < b.subject;
  if (a.requirement != b.requirement) return a.requirement < b.requirement;
  return a.detail < b.detail;
}

bool CompatibilityAssessment::admits_placement() const noexcept {
  return is_compatible_family(overall) && policy_admissible != Tri::No;
}

bool CompatibilityAssessment::requires_adaptation() const noexcept {
  switch (overall) {
    case CompatibilityOutcome::CompatibleWithFallback:
    case CompatibilityOutcome::CompatibleAfterReconfiguration:
    case CompatibilityOutcome::CompatibleAfterRebuild:
      return true;
    default:
      return false;
  }
}

const CompatibilityCheck* CompatibilityAssessment::primary_failure() const noexcept {
  const CompatibilityCheck* worst = nullptr;
  int worst_severity = -1;
  for (const CompatibilityCheck& check : checks) {
    if (compatibility_severity(check.outcome) <= 3) {
      continue;
    }
    const int severity = compatibility_severity(check.outcome);
    if (severity > worst_severity) {
      worst_severity = severity;
      worst = &check;
    }
  }
  return worst;
}

std::string CompatibilityAssessment::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"overall", std::string(fo::to_string(overall))});
  rows.push_back({"admits_placement", admits_placement() ? "yes" : "no"});
  rows.push_back({"requires_adaptation", requires_adaptation() ? "yes" : "no"});
  rows.push_back({"target_cluster", render_id(target_cluster.value())});
  rows.push_back({"target_cluster_generation",
                  target_cluster_generation.is_set() ? target_cluster_generation.to_string() : "-"});
  rows.push_back({"target_cluster_epoch",
                  target_cluster_epoch.is_set() ? target_cluster_epoch.to_string() : "-"});
  rows.push_back({"target_accelerator_class", render_id(target_accelerator_class.value())});
  rows.push_back({"workload", render_id(workload.value())});
  rows.push_back({"workload_generation",
                  workload_generation.is_set() ? workload_generation.to_string() : "-"});
  rows.push_back({"artifact", render_id(artifact.value())});
  rows.push_back({"artifact_generation",
                  artifact_generation.is_set() ? artifact_generation.to_string() : "-"});
  rows.push_back({"runtime", render_id(runtime.value())});
  rows.push_back({"compatibility_generation",
                  compatibility_generation.is_set() ? compatibility_generation.to_string() : "-"});
  rows.push_back({"target_currentness", std::string(fo::to_string(target_currentness))});
  rows.push_back({"policy_admissible", std::string(fo::to_string(policy_admissible))});
  rows.push_back({"policy_detail", policy_detail.empty() ? "-" : policy_detail});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = std::string(indent) + "compatibility:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!checks.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> check_rows;
    for (const CompatibilityCheck& check : checks) {
      check_rows.push_back({std::string(fo::to_string(check.subject)),
                            std::string(fo::to_string(check.outcome)), check.requirement,
                            check.observed, check.detail});
    }
    out += render_table({"relationship", "outcome", "requirement", "observed", "detail"}, check_rows,
                        std::string(indent) + "  ");
    out += '\n';
  }
  return out;
}

CompatibilityOutcome compare_runtime_versions(std::string_view required_runtime_abi,
                                              std::string_view observed_runtime_abi,
                                              std::string_view required_driver_abi,
                                              std::string_view observed_driver_abi) noexcept {
  if (!required_runtime_abi.empty()) {
    if (observed_runtime_abi.empty()) {
      return CompatibilityOutcome::UnknownCompatibility;
    }
    if (required_runtime_abi != observed_runtime_abi) {
      return CompatibilityOutcome::IncompatibleAbi;
    }
  }
  if (!required_driver_abi.empty()) {
    if (observed_driver_abi.empty()) {
      return CompatibilityOutcome::UnknownCompatibility;
    }
    if (required_driver_abi != observed_driver_abi) {
      return CompatibilityOutcome::IncompatibleDriver;
    }
  }
  return CompatibilityOutcome::Compatible;
}

CompatibilityAssessment assess_compatibility(const CompatibilityInputs& inputs) {
  Builder builder;
  builder.inputs = &inputs;
  CompatibilityAssessment& a = builder.assessment;

  // Start from the strongest verdict and let every check weaken it. Seeding with the
  // weakest verdict would make every assessment permanently undecided.
  a.overall = CompatibilityOutcome::Compatible;
  a.compatibility_generation = inputs.compatibility_generation;
  a.target_currentness = inputs.target_currentness;
  a.policy_admissible = Tri::Unknown;

  if (inputs.workload != nullptr) {
    a.workload = inputs.workload->id;
    a.workload_generation = inputs.workload->generation;
  }
  if (inputs.artifact != nullptr) {
    a.artifact = inputs.artifact->id;
    a.artifact_generation = inputs.artifact->generation;
  }
  if (inputs.cluster != nullptr) {
    a.target_cluster = inputs.cluster->id;
    a.target_cluster_generation = inputs.cluster->generation;
    a.target_cluster_epoch = inputs.cluster->epoch;
  }
  if (inputs.accelerator_class != nullptr) {
    a.target_accelerator_class = inputs.accelerator_class->id;
  }
  if (inputs.cluster_runtime != nullptr) {
    a.runtime = inputs.cluster_runtime->id;
    a.runtime_generation = inputs.cluster_runtime->generation;
  }

  if (inputs.cluster == nullptr) {
    builder.add(CompatibilitySubject::ClusterCurrentness, "target cluster record", "absent",
                CompatibilityOutcome::UnknownCompatibility,
                "no cluster record was supplied; compatibility cannot be assessed");
    a.precision = Precision::Unknown;
    a.evidence_class = EvidenceClass::Unknown;
    return a;
  }

  // 1. Freshness of the target's evidence. A stale target cannot be called compatible.
  switch (inputs.target_currentness) {
    case Currentness::Current:
      builder.add(CompatibilitySubject::ClusterCurrentness, "evidence current", "CURRENT",
                  CompatibilityOutcome::Compatible, "target evidence is current");
      break;
    case Currentness::RevalidationRequired:
      builder.add(CompatibilitySubject::ClusterCurrentness, "evidence current",
                  "REVALIDATION_REQUIRED", CompatibilityOutcome::StaleEvidence,
                  "target was restored from persistence and has not republished evidence");
      break;
    case Currentness::Stale:
      builder.add(CompatibilitySubject::ClusterCurrentness, "evidence current", "STALE",
                  CompatibilityOutcome::StaleEvidence,
                  "target evidence is stale; a conclusion cannot be asserted");
      break;
    case Currentness::Retired:
      builder.add(CompatibilitySubject::ClusterCurrentness, "evidence current", "RETIRED",
                  CompatibilityOutcome::Unsupported, "target cluster is retired");
      break;
    case Currentness::Unknown:
      builder.add(CompatibilitySubject::ClusterCurrentness, "evidence current", "UNKNOWN",
                  CompatibilityOutcome::UnknownCompatibility,
                  "freshness of the target evidence is unknown");
      break;
  }

  // 2. Artifact against accelerator architecture.
  if (inputs.artifact != nullptr && is_code_bearing(inputs.artifact->kind)) {
    if (inputs.artifact->target_architectures.empty()) {
      builder.add(CompatibilitySubject::ArtifactArchitecture, "artifact declares a target architecture",
                  "UNKNOWN", CompatibilityOutcome::UnknownCompatibility,
                  "the artifact does not declare any target architecture");
    } else if (inputs.accelerator_class == nullptr || inputs.accelerator_class->architecture.empty()) {
      builder.add(CompatibilitySubject::ArtifactArchitecture,
                  join_strings(inputs.artifact->target_architectures, ","), "UNKNOWN",
                  CompatibilityOutcome::UnknownCompatibility,
                  "the target accelerator architecture is unpublished");
    } else if (!inputs.artifact->targets_architecture(inputs.accelerator_class->architecture)) {
      builder.add(CompatibilitySubject::ArtifactArchitecture,
                  join_strings(inputs.artifact->target_architectures, ","),
                  inputs.accelerator_class->architecture,
                  CompatibilityOutcome::IncompatibleArchitecture,
                  "the artifact targets no binary compatible with this accelerator architecture");
    } else {
      builder.add(CompatibilitySubject::ArtifactArchitecture,
                  join_strings(inputs.artifact->target_architectures, ","),
                  inputs.accelerator_class->architecture, CompatibilityOutcome::Compatible,
                  "the artifact declares this accelerator architecture as a target");
    }

    if (!inputs.artifact->minimum_compute_capability.empty()) {
      if (inputs.accelerator_class == nullptr || inputs.accelerator_class->compute_capability.empty()) {
        builder.add(CompatibilitySubject::ArtifactArchitecture,
                    "compute capability >= " + inputs.artifact->minimum_compute_capability, "UNKNOWN",
                    CompatibilityOutcome::UnknownCompatibility,
                    "the target compute capability is unpublished");
      } else {
        const int order = compare_version_strings(inputs.accelerator_class->compute_capability,
                                                  inputs.artifact->minimum_compute_capability);
        builder.add(CompatibilitySubject::ArtifactArchitecture,
                    "compute capability >= " + inputs.artifact->minimum_compute_capability,
                    inputs.accelerator_class->compute_capability,
                    order >= 0 ? CompatibilityOutcome::Compatible
                               : CompatibilityOutcome::IncompatibleArchitecture,
                    order >= 0 ? "the target meets the artifact minimum compute capability"
                               : "the target is below the artifact minimum compute capability");
      }
    }
  }

  // 3. Artifact-declared capability requirements.
  if (inputs.artifact != nullptr) {
    if (!inputs.artifact->format.empty()) {
      CapabilityRequirement requirement;
      requirement.key.key = CapabilityKey::ArtifactFormats;
      requirement.comparator = CapabilityComparator::AnyOf;
      CapabilityValue::TextSet formats;
      formats.push_back(inputs.artifact->format);
      const Result<CapabilityValue> value = CapabilityValue::text_set(std::move(formats));
      if (value.ok()) {
        requirement.value = value.value();
        requirement.rationale = "artifact format";
        builder.add_capability_requirement(requirement, true);
      }
    }
    if (!inputs.artifact->kernel_format.empty()) {
      CapabilityRequirement requirement;
      requirement.key.key = CapabilityKey::KernelFormat;
      requirement.comparator = CapabilityComparator::Equals;
      requirement.value = CapabilityValue::text(inputs.artifact->kernel_format);
      requirement.rationale = "artifact kernel format";
      builder.add_capability_requirement(requirement, true);
    }
    if (!inputs.artifact->compiler_target.empty()) {
      CapabilityRequirement requirement;
      requirement.key.key = CapabilityKey::CompilerTarget;
      requirement.comparator = CapabilityComparator::Equals;
      requirement.value = CapabilityValue::text(inputs.artifact->compiler_target);
      requirement.rationale = "artifact compiler target";
      builder.add_capability_requirement(requirement, true);
    }
  }

  // 4. Artifact ABI against the runtime/driver ABI.
  if (inputs.artifact != nullptr &&
      (!inputs.artifact->runtime_abi.empty() || !inputs.artifact->driver_abi.empty())) {
    const std::string observed_runtime =
        inputs.cluster_runtime != nullptr ? inputs.cluster_runtime->abi : std::string();
    const std::string observed_driver =
        inputs.cluster_runtime != nullptr ? inputs.cluster_runtime->driver_abi : std::string();
    const CompatibilityOutcome outcome = compare_runtime_versions(
        inputs.artifact->runtime_abi, observed_runtime, inputs.artifact->driver_abi, observed_driver);
    std::string detail;
    switch (outcome) {
      case CompatibilityOutcome::Compatible:
        detail = "runtime and driver ABIs match the artifact declaration";
        break;
      case CompatibilityOutcome::IncompatibleAbi:
        detail = "the artifact requires runtime ABI " + inputs.artifact->runtime_abi +
                 ", the target publishes " + render_id(observed_runtime);
        break;
      case CompatibilityOutcome::IncompatibleDriver:
        detail = "the artifact requires driver ABI " + inputs.artifact->driver_abi +
                 ", the target publishes " + render_id(observed_driver);
        break;
      default:
        detail = "the target does not publish an ABI to compare";
        break;
    }
    builder.add(CompatibilitySubject::ArtifactRuntime, inputs.artifact->runtime_abi,
                render_id(observed_runtime), outcome, detail);
    builder.add(CompatibilitySubject::ArtifactDriver, inputs.artifact->driver_abi,
                render_id(observed_driver), outcome, detail);
  }

  // 5. Workload-declared capability requirements.
  if (inputs.workload != nullptr) {
    for (const CapabilityRequirement& requirement : inputs.workload->requirements) {
      builder.add_capability_requirement(requirement, false);
    }
  }

  // 6. Memory requirement against device memory.
  if (inputs.workload != nullptr && inputs.workload->required_memory_bytes_per_accelerator > 0) {
    if (inputs.accelerator_class == nullptr || inputs.accelerator_class->memory_bytes_per_device == 0) {
      builder.add(CompatibilitySubject::MemoryRequirement,
                  std::to_string(inputs.workload->required_memory_bytes_per_accelerator) + " bytes",
                  "UNKNOWN", CompatibilityOutcome::UnknownCompatibility,
                  "per-device memory capacity is unpublished for the target");
    } else if (inputs.workload->required_memory_bytes_per_accelerator >
               inputs.accelerator_class->memory_bytes_per_device) {
      builder.add(CompatibilitySubject::MemoryRequirement,
                  std::to_string(inputs.workload->required_memory_bytes_per_accelerator) + " bytes",
                  std::to_string(inputs.accelerator_class->memory_bytes_per_device) + " bytes",
                  CompatibilityOutcome::IncompatibleMemory,
                  "the workload requires more per-device memory than the target provides");
    } else {
      builder.add(CompatibilitySubject::MemoryRequirement,
                  std::to_string(inputs.workload->required_memory_bytes_per_accelerator) + " bytes",
                  std::to_string(inputs.accelerator_class->memory_bytes_per_device) + " bytes",
                  CompatibilityOutcome::Compatible,
                  "per-device memory is sufficient for this workload generation");
    }
  }
  // A workload generation that states no per-device memory requirement does not constrain
  // memory at all, so no memory check is emitted. Reporting UNKNOWN here would make every
  // such workload permanently undecided, which is not what the evidence says.

  // 7. Isolation requirement.
  if (inputs.workload != nullptr && !inputs.workload->isolation_requirement.empty()) {
    CapabilityRequirement requirement;
    requirement.key.key = CapabilityKey::SecurityIsolation;
    requirement.comparator = CapabilityComparator::AnyOf;
    CapabilityValue::TextSet allowed;
    allowed.push_back(inputs.workload->isolation_requirement);
    const Result<CapabilityValue> value = CapabilityValue::text_set(std::move(allowed));
    if (value.ok()) {
      requirement.value = value.value();
      requirement.rationale = "workload isolation requirement";
      builder.add_capability_requirement(requirement, false);
    }
  }

  // 8. Policy admissibility, tracked outside the compatibility verdict.
  if (inputs.policy != nullptr) {
    const Tri site_permitted = inputs.policy->permits_site(inputs.cluster->site);
    const Tri cluster_permitted = inputs.policy->permits_cluster(inputs.cluster->id);
    a.policy_admissible = tri_and(site_permitted, cluster_permitted);
    switch (a.policy_admissible) {
      case Tri::Yes:
        a.policy_detail = "policy " + inputs.policy->id.value() + " generation " +
                          inputs.policy->generation.to_string() + " permits this target";
        break;
      case Tri::No:
        a.policy_detail = "policy " + inputs.policy->id.value() + " generation " +
                          inputs.policy->generation.to_string() + " does not permit this target";
        break;
      case Tri::Unknown:
        a.policy_detail = "policy admissibility could not be decided from the supplied evidence";
        break;
    }
    if (a.policy_admissible == Tri::Yes) {
      builder.add(CompatibilitySubject::PolicyAdmissibility, "policy permits the target",
                  inputs.policy->id.value(), CompatibilityOutcome::Compatible, a.policy_detail);
    }
  } else {
    a.policy_detail = "no policy record was supplied; admissibility is not evaluated here";
  }

  // Aggregate precision and evidence class from the participating records.
  Precision precision = Precision::Derived;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  bool first = true;
  const auto consider = [&](Precision p, EvidenceClass c) {
    if (first) {
      precision = p;
      evidence_class = c;
      first = false;
      return;
    }
    precision = weakest(precision, p);
    evidence_class = weaker(evidence_class, c);
  };
  if (inputs.cluster != nullptr) {
    consider(inputs.cluster->stamp.precision, inputs.cluster->stamp.evidence_class);
  }
  if (inputs.accelerator_class != nullptr) {
    consider(inputs.accelerator_class->stamp.precision, inputs.accelerator_class->stamp.evidence_class);
  }
  if (inputs.artifact != nullptr) {
    consider(inputs.artifact->stamp.precision, inputs.artifact->stamp.evidence_class);
  }
  if (inputs.workload != nullptr) {
    consider(inputs.workload->stamp.precision, inputs.workload->stamp.evidence_class);
  }
  if (inputs.effective_capabilities != nullptr && !inputs.effective_capabilities->empty()) {
    consider(inputs.effective_capabilities->combined_precision(),
             inputs.effective_capabilities->combined_class());
  }
  a.precision = precision;
  a.evidence_class = evidence_class;

  const Status evidence_status = a.evidence.add(
      Provenance::DerivedAnalysis, evidence_class, "compatibility-assessor", Precision::Derived,
      "assessed " + std::to_string(a.checks.size()) + " relationships for cluster " +
          a.target_cluster.value() + " workload generation " +
          (a.workload_generation.is_set() ? a.workload_generation.to_string() : std::string("-")));
  (void)evidence_status;
  return a;
}

}  // namespace fo
