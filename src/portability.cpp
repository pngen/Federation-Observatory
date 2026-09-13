// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Portability is evaluated per dimension. A single boolean can never express the
// difference between "runs as-is", "runs after a rebuild", and "runs only if the
// checkpoint format is translated".

#include <algorithm>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/portability.hpp"

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

std::string architecture_list(const ArtifactRecord& artifact) {
  return artifact.target_architectures.empty() ? std::string("UNKNOWN")
                                               : join_strings(artifact.target_architectures, ",");
}

const CapabilityEntry* capability_of(const CapabilitySet* set, CapabilityKey key) {
  if (set == nullptr) {
    return nullptr;
  }
  CapabilityRef ref;
  ref.key = key;
  return set->find(ref);
}

bool destination_supports_text(const CapabilitySet* set, CapabilityKey key, std::string_view value,
                               bool& decided) {
  const CapabilityEntry* entry = capability_of(set, key);
  if (entry == nullptr || !entry->value.known()) {
    decided = false;
    return false;
  }
  if (entry->value.is_text()) {
    decided = true;
    return entry->value.as_text() == value;
  }
  if (entry->value.is_text_set()) {
    decided = true;
    const auto& set_values = entry->value.as_text_set();
    return std::find(set_values.begin(), set_values.end(), value) != set_values.end();
  }
  decided = false;
  return false;
}

}  // namespace

std::string_view to_string(PortabilityDimension dimension) noexcept {
  switch (dimension) {
    case PortabilityDimension::Binary: return "BINARY";
    case PortabilityDimension::Artifact: return "ARTIFACT";
    case PortabilityDimension::Model: return "MODEL";
    case PortabilityDimension::Checkpoint: return "CHECKPOINT";
    case PortabilityDimension::RuntimeApi: return "RUNTIME_API";
    case PortabilityDimension::Kernel: return "KERNEL";
    case PortabilityDimension::DataFormat: return "DATA_FORMAT";
    case PortabilityDimension::State: return "STATE";
    case PortabilityDimension::NumericalSemantic: return "NUMERICAL_SEMANTIC";
    case PortabilityDimension::Performance: return "PERFORMANCE";
    case PortabilityDimension::Operational: return "OPERATIONAL";
  }
  return "BINARY";
}

bool parse_portability_dimension(std::string_view text, PortabilityDimension& out) noexcept {
  for (std::size_t i = 0; i < kPortabilityDimensionCount; ++i) {
    const auto candidate = static_cast<PortabilityDimension>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(PortabilityOutcome outcome) noexcept {
  switch (outcome) {
    case PortabilityOutcome::PortableDirect: return "PORTABLE_DIRECT";
    case PortabilityOutcome::PortableWithFallback: return "PORTABLE_WITH_FALLBACK";
    case PortabilityOutcome::PortableWithRebuild: return "PORTABLE_WITH_REBUILD";
    case PortabilityOutcome::PortableWithRecompile: return "PORTABLE_WITH_RECOMPILE";
    case PortabilityOutcome::PortableWithConversion: return "PORTABLE_WITH_CONVERSION";
    case PortabilityOutcome::PortableWithStateTranslation: return "PORTABLE_WITH_STATE_TRANSLATION";
    case PortabilityOutcome::NotPortableTopology: return "NOT_PORTABLE_TOPOLOGY";
    case PortabilityOutcome::NotPortablePolicy: return "NOT_PORTABLE_POLICY";
    case PortabilityOutcome::NotPortableArtifact: return "NOT_PORTABLE_ARTIFACT";
    case PortabilityOutcome::NotPortableRuntime: return "NOT_PORTABLE_RUNTIME";
    case PortabilityOutcome::NotPortableState: return "NOT_PORTABLE_STATE";
    case PortabilityOutcome::NotPortableArchitecture: return "NOT_PORTABLE_ARCHITECTURE";
    case PortabilityOutcome::Unknown: return "UNKNOWN";
    case PortabilityOutcome::Unsupported: return "UNSUPPORTED";
  }
  return "UNKNOWN";
}

bool parse_portability_outcome(std::string_view text, PortabilityOutcome& out) noexcept {
  for (std::size_t i = 0; i < kPortabilityOutcomeCount; ++i) {
    const auto candidate = static_cast<PortabilityOutcome>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

int portability_severity(PortabilityOutcome outcome) noexcept {
  switch (outcome) {
    case PortabilityOutcome::PortableDirect: return 0;
    case PortabilityOutcome::PortableWithFallback: return 1;
    case PortabilityOutcome::PortableWithRebuild: return 2;
    case PortabilityOutcome::PortableWithRecompile: return 3;
    case PortabilityOutcome::PortableWithConversion: return 4;
    case PortabilityOutcome::PortableWithStateTranslation: return 5;
    case PortabilityOutcome::NotPortableTopology: return 6;
    case PortabilityOutcome::NotPortablePolicy: return 7;
    case PortabilityOutcome::NotPortableArtifact: return 8;
    case PortabilityOutcome::NotPortableRuntime: return 9;
    case PortabilityOutcome::NotPortableState: return 10;
    case PortabilityOutcome::NotPortableArchitecture: return 11;
    case PortabilityOutcome::Unknown: return 12;
    case PortabilityOutcome::Unsupported: return 13;
  }
  return 12;
}

PortabilityOutcome weakest(PortabilityOutcome a, PortabilityOutcome b) noexcept {
  return portability_severity(a) >= portability_severity(b) ? a : b;
}

bool requires_adaptation(PortabilityOutcome outcome) noexcept {
  switch (outcome) {
    case PortabilityOutcome::PortableWithRebuild:
    case PortabilityOutcome::PortableWithRecompile:
    case PortabilityOutcome::PortableWithConversion:
    case PortabilityOutcome::PortableWithStateTranslation:
    case PortabilityOutcome::PortableWithFallback:
      return true;
    default:
      return false;
  }
}

bool is_technical_failure(PortabilityOutcome outcome) noexcept {
  switch (outcome) {
    case PortabilityOutcome::NotPortableTopology:
    case PortabilityOutcome::NotPortableArtifact:
    case PortabilityOutcome::NotPortableRuntime:
    case PortabilityOutcome::NotPortableState:
    case PortabilityOutcome::NotPortableArchitecture:
      return true;
    default:
      return false;
  }
}

bool is_policy_failure(PortabilityOutcome outcome) noexcept {
  return outcome == PortabilityOutcome::NotPortablePolicy;
}

bool is_undecided(PortabilityOutcome outcome) noexcept {
  return outcome == PortabilityOutcome::Unknown || outcome == PortabilityOutcome::Unsupported;
}

bool operator<(const PortabilityDimensionResult& a, const PortabilityDimensionResult& b) noexcept {
  if (a.dimension != b.dimension) return a.dimension < b.dimension;
  return a.detail < b.detail;
}

Status PortabilityAssessment::validate() const {
  if (workload.empty()) {
    return fail(ErrorCode::InvalidArgument, "portability assessment has no workload");
  }
  if (!workload_generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "portability workload generation is unset",
                workload.value());
  }
  if (destination.empty()) {
    return fail(ErrorCode::InvalidArgument, "portability assessment has no destination",
                workload.value());
  }
  if (dimensions.size() > kPortabilityDimensionCount) {
    return fail(ErrorCode::BoundExceeded, "too many portability dimensions", workload.value());
  }
  std::vector<PortabilityDimension> seen;
  seen.reserve(dimensions.size());
  for (const PortabilityDimensionResult& result : dimensions) {
    seen.push_back(result.dimension);
  }
  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
    return fail(ErrorCode::InvalidArgument, "duplicate portability dimension", workload.value());
  }
  return Status::success();
}

const PortabilityDimensionResult* PortabilityAssessment::find(PortabilityDimension dimension) const {
  for (const PortabilityDimensionResult& result : dimensions) {
    if (result.dimension == dimension) {
      return &result;
    }
  }
  return nullptr;
}

std::string PortabilityAssessment::digest() const { return digest_text(render()); }

std::string PortabilityAssessment::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"workload", render_id(workload.value())});
  rows.push_back({"workload_generation",
                  workload_generation.is_set() ? workload_generation.to_string() : "-"});
  rows.push_back({"artifact", render_id(artifact.value())});
  rows.push_back({"artifact_generation",
                  artifact_generation.is_set() ? artifact_generation.to_string() : "-"});
  rows.push_back({"source", render_id(source.value())});
  rows.push_back({"source_generation", source_generation.is_set() ? source_generation.to_string() : "-"});
  rows.push_back({"destination", render_id(destination.value())});
  rows.push_back({"destination_generation",
                  destination_generation.is_set() ? destination_generation.to_string() : "-"});
  rows.push_back({"source_runtime", render_id(source_runtime.value())});
  rows.push_back({"destination_runtime", render_id(destination_runtime.value())});
  rows.push_back({"overall", std::string(fo::to_string(overall))});
  rows.push_back({"requires_rebuild", requires_rebuild ? "yes" : "no"});
  rows.push_back({"requires_recompile", requires_recompile ? "yes" : "no"});
  rows.push_back({"requires_conversion", requires_conversion ? "yes" : "no"});
  rows.push_back({"requires_state_translation", requires_state_translation ? "yes" : "no"});
  rows.push_back({"requires_fallback", requires_fallback ? "yes" : "no"});
  rows.push_back({"technically_blocked", technically_blocked ? "yes" : "no"});
  rows.push_back({"policy_blocked", policy_blocked ? "yes" : "no"});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = std::string(indent) + "portability:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!dimensions.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> dim_rows;
    for (const PortabilityDimensionResult& result : dimensions) {
      dim_rows.push_back({std::string(fo::to_string(result.dimension)),
                          std::string(fo::to_string(result.outcome)),
                          std::string(fo::to_string(result.basis)),
                          std::string(fo::to_string(result.precision)), result.detail});
    }
    out += render_table({"dimension", "outcome", "basis", "precision", "detail"}, dim_rows,
                        std::string(indent) + "  ");
    out += '\n';
  }
  return out;
}

PortabilityAssessment assess_portability(const PortabilityInputs& inputs) {
  PortabilityAssessment assessment;
  assessment.federation = inputs.federation;
  assessment.federation_generation = inputs.federation_generation;
  assessment.compatibility_generation = inputs.compatibility_generation;
  assessment.policy_generation = inputs.policy_generation;

  if (inputs.workload != nullptr) {
    assessment.workload = inputs.workload->id;
    assessment.workload_generation = inputs.workload->generation;
    assessment.workload_class = inputs.workload->workload_class;
  }
  if (inputs.artifact != nullptr) {
    assessment.artifact = inputs.artifact->id;
    assessment.artifact_generation = inputs.artifact->generation;
  }
  if (inputs.source_cluster != nullptr) {
    assessment.source = inputs.source_cluster->id;
    assessment.source_generation = inputs.source_cluster->generation;
    assessment.source_epoch = inputs.source_cluster->epoch;
    assessment.currentness = inputs.source_cluster->currentness;
  }
  if (inputs.source_runtime != nullptr) {
    assessment.source_runtime = inputs.source_runtime->id;
    assessment.source_runtime_generation = inputs.source_runtime->generation;
  }
  if (inputs.destination_cluster != nullptr) {
    assessment.destination = inputs.destination_cluster->id;
    assessment.destination_generation = inputs.destination_cluster->generation;
    assessment.destination_epoch = inputs.destination_cluster->epoch;
    assessment.currentness = inputs.destination_cluster->currentness;
  }
  if (inputs.destination_runtime != nullptr) {
    assessment.destination_runtime = inputs.destination_runtime->id;
    assessment.destination_runtime_generation = inputs.destination_runtime->generation;
  }

  const auto record = [&assessment](PortabilityDimension dimension, PortabilityOutcome outcome,
                                    std::string detail, ReasonBasis basis, Precision precision,
                                    EvidenceClass evidence_class,
                                    bool constrains_overall = true) {
    PortabilityDimensionResult result;
    result.dimension = dimension;
    result.outcome = outcome;
    result.constrains_overall = constrains_overall;
    result.detail = std::move(detail);
    result.basis = basis;
    result.precision = precision;
    result.evidence_class = evidence_class;
    assessment.dimensions.push_back(std::move(result));
  };

  const Precision derived_precision = Precision::Derived;
  const EvidenceClass derived_class = inputs.destination_cluster != nullptr
                                          ? inputs.destination_cluster->stamp.evidence_class
                                          : EvidenceClass::Unknown;
  if (inputs.destination_cluster == nullptr) {
    for (std::size_t i = 0; i < kPortabilityDimensionCount; ++i) {
      record(static_cast<PortabilityDimension>(i), PortabilityOutcome::Unknown,
             "no destination cluster was supplied", ReasonBasis::Unattributed, Precision::Unknown,
             EvidenceClass::Unknown);
    }
    assessment.overall = PortabilityOutcome::Unknown;
    assessment.precision = Precision::Unknown;
    assessment.evidence_class = EvidenceClass::Unknown;
    const Status s = assessment.evidence.add(Provenance::DerivedAnalysis, EvidenceClass::Unknown,
                                             "portability-assessor", Precision::Unknown,
                                             "destination cluster absent");
    (void)s;
    return assessment;
  }

  // --- Binary -------------------------------------------------------------
  if (inputs.artifact == nullptr || !is_code_bearing(inputs.artifact->kind)) {
    record(PortabilityDimension::Binary, PortabilityOutcome::Unknown,
           "binary portability does not apply to this artifact kind, or no artifact is known",
           ReasonBasis::Unattributed, Precision::Unknown, derived_class, false);
  } else if (inputs.artifact->target_architectures.empty()) {
    record(PortabilityDimension::Binary, PortabilityOutcome::Unknown,
           "the artifact does not declare its target architectures",
           ReasonBasis::Observed, inputs.artifact->stamp.precision, inputs.artifact->stamp.evidence_class);
  } else if (inputs.destination_accelerator_class == nullptr ||
             inputs.destination_accelerator_class->architecture.empty()) {
    record(PortabilityDimension::Binary, PortabilityOutcome::Unknown,
           "the destination accelerator architecture is unpublished", ReasonBasis::Observed,
           Precision::Unknown, derived_class);
  } else if (inputs.artifact->targets_architecture(
                 inputs.destination_accelerator_class->architecture)) {
    record(PortabilityDimension::Binary, PortabilityOutcome::PortableDirect,
           "the artifact declares the destination architecture " +
               inputs.destination_accelerator_class->architecture + " (targets: " +
               architecture_list(*inputs.artifact) + ")",
           ReasonBasis::Derived, derived_precision, derived_class);
  } else {
    bool decided = false;
    const bool can_rebuild =
        !inputs.artifact->compiler_target.empty() &&
        destination_supports_text(inputs.destination_capabilities, CapabilityKey::CompilerTarget,
                                  inputs.artifact->compiler_target, decided);
    if (can_rebuild) {
      record(PortabilityDimension::Binary, PortabilityOutcome::PortableWithRebuild,
             "the artifact targets " + architecture_list(*inputs.artifact) +
                 " but the destination publishes compiler target " +
                 inputs.artifact->compiler_target,
             ReasonBasis::Derived, derived_precision, derived_class);
    } else {
      record(PortabilityDimension::Binary, PortabilityOutcome::NotPortableArchitecture,
             "the artifact targets " + architecture_list(*inputs.artifact) +
                 " and not the destination architecture " +
                 inputs.destination_accelerator_class->architecture +
                 (decided ? "; the destination does not publish a matching compiler target"
                          : "; no destination compiler target is published"),
             ReasonBasis::Derived, derived_precision, derived_class);
    }
  }

  // --- Artifact / DataFormat ---------------------------------------------
  if (inputs.artifact == nullptr || inputs.artifact->format.empty()) {
    record(PortabilityDimension::Artifact, PortabilityOutcome::Unknown,
           "the artifact does not declare a format", ReasonBasis::Unattributed, Precision::Unknown,
           derived_class, false);
    record(PortabilityDimension::DataFormat, PortabilityOutcome::Unknown,
           "no data format evidence is available", ReasonBasis::Unattributed, Precision::Unknown,
           derived_class, false);
  } else {
    bool decided = false;
    const bool supported = destination_supports_text(inputs.destination_capabilities,
                                                     CapabilityKey::ArtifactFormats,
                                                     inputs.artifact->format, decided);
    const PortabilityOutcome outcome = !decided ? PortabilityOutcome::Unknown
                                      : supported ? PortabilityOutcome::PortableDirect
                                                  : PortabilityOutcome::NotPortableArtifact;
    const std::string detail =
        !decided ? "the destination does not publish supported artifact formats"
                 : supported ? "the destination supports artifact format " + inputs.artifact->format
                             : "the destination does not support artifact format " +
                                   inputs.artifact->format;
    record(PortabilityDimension::Artifact, outcome, detail, ReasonBasis::Derived, derived_precision,
           derived_class);
    record(PortabilityDimension::DataFormat, outcome, detail, ReasonBasis::Derived,
           derived_precision, derived_class);
  }

  // --- Model --------------------------------------------------------------
  if (inputs.artifact == nullptr || (inputs.artifact->kind != ArtifactKind::ModelWeights &&
                                     inputs.artifact->kind != ArtifactKind::Adapter)) {
    record(PortabilityDimension::Model, PortabilityOutcome::Unknown,
           "model portability does not apply to this artifact kind, or no artifact is known",
           ReasonBasis::Unattributed, Precision::Unknown, derived_class, false);
  } else {
    bool decided = false;
    const bool supported = destination_supports_text(inputs.destination_capabilities,
                                                     CapabilityKey::InferenceServingBackend,
                                                     inputs.artifact->format, decided);
    record(PortabilityDimension::Model,
           !decided ? PortabilityOutcome::Unknown
                    : supported ? PortabilityOutcome::PortableDirect
                                : PortabilityOutcome::NotPortableArtifact,
           !decided ? "the destination does not publish a serving backend capability"
                    : supported ? "the destination serving backend accepts format " +
                                      inputs.artifact->format
                                : "no destination serving backend accepts format " +
                                      inputs.artifact->format,
           ReasonBasis::Derived, derived_precision, derived_class);
  }

  // --- Checkpoint / State -------------------------------------------------
  if (inputs.artifact == nullptr || inputs.artifact->state_format.empty()) {
    record(PortabilityDimension::Checkpoint, PortabilityOutcome::Unknown,
           "the artifact does not declare a state format", ReasonBasis::Unattributed,
           Precision::Unknown, derived_class, false);
    record(PortabilityDimension::State, PortabilityOutcome::Unknown,
           "state portability cannot be decided without a declared state format",
           ReasonBasis::Unattributed, Precision::Unknown, derived_class, false);
  } else {
    bool decided = false;
    const bool supported = destination_supports_text(inputs.destination_capabilities,
                                                     CapabilityKey::ArtifactFormats,
                                                     inputs.artifact->state_format, decided);
    if (!decided) {
      record(PortabilityDimension::Checkpoint, PortabilityOutcome::Unknown,
             "the destination does not publish supported state formats", ReasonBasis::Derived,
             derived_precision, derived_class);
      record(PortabilityDimension::State, PortabilityOutcome::Unknown,
             "state portability cannot be decided", ReasonBasis::Derived, derived_precision,
             derived_class);
    } else if (supported) {
      record(PortabilityDimension::Checkpoint, PortabilityOutcome::PortableDirect,
             "the destination accepts checkpoint format " + inputs.artifact->state_format,
             ReasonBasis::Derived, derived_precision, derived_class);
      record(PortabilityDimension::State, PortabilityOutcome::PortableDirect,
             "the destination accepts checkpoint format " + inputs.artifact->state_format,
             ReasonBasis::Derived, derived_precision, derived_class);
    } else {
      record(PortabilityDimension::Checkpoint,
             PortabilityOutcome::PortableWithStateTranslation,
             "checkpoint format " + inputs.artifact->state_format +
                 " is not accepted by the destination; a translation step is required",
             ReasonBasis::Derived, derived_precision, derived_class);
      record(PortabilityDimension::State, PortabilityOutcome::NotPortableState,
             "state captured in format " + inputs.artifact->state_format +
                 " is not directly restorable at the destination",
             ReasonBasis::Derived, derived_precision, derived_class);
    }
  }

  // --- RuntimeApi ---------------------------------------------------------
  if (inputs.source_runtime == nullptr || inputs.destination_runtime == nullptr) {
    record(PortabilityDimension::RuntimeApi, PortabilityOutcome::Unknown,
           "source and destination runtimes are not both known", ReasonBasis::Unattributed,
           Precision::Unknown, derived_class);
  } else if (inputs.source_runtime->kind != inputs.destination_runtime->kind) {
    record(PortabilityDimension::RuntimeApi, PortabilityOutcome::NotPortableRuntime,
           std::string("runtime API family changes from ") +
               std::string(fo::to_string(inputs.source_runtime->kind)) + " to " +
               std::string(fo::to_string(inputs.destination_runtime->kind)),
           ReasonBasis::Derived, derived_precision, derived_class);
  } else if (inputs.source_runtime->abi != inputs.destination_runtime->abi ||
             inputs.source_runtime->generation != inputs.destination_runtime->generation) {
    record(PortabilityDimension::RuntimeApi, PortabilityOutcome::PortableWithRecompile,
           "runtime ABI generation changes from " + render_id(inputs.source_runtime->abi) + " (" +
               inputs.source_runtime->generation.to_string() + ") to " +
               render_id(inputs.destination_runtime->abi) + " (" +
               inputs.destination_runtime->generation.to_string() + ")",
           ReasonBasis::Derived, derived_precision, derived_class);
  } else {
    record(PortabilityDimension::RuntimeApi, PortabilityOutcome::PortableDirect,
           "source and destination publish the same runtime kind, ABI and generation",
           ReasonBasis::Derived, derived_precision, derived_class);
  }

  // --- Kernel -------------------------------------------------------------
  if (inputs.artifact == nullptr || inputs.artifact->kernel_format.empty()) {
    record(PortabilityDimension::Kernel, PortabilityOutcome::Unknown,
           "the artifact does not declare a kernel format", ReasonBasis::Unattributed,
           Precision::Unknown, derived_class, false);
  } else {
    bool decided = false;
    const bool supported = destination_supports_text(inputs.destination_capabilities,
                                                     CapabilityKey::KernelFormat,
                                                     inputs.artifact->kernel_format, decided);
    if (!decided) {
      record(PortabilityDimension::Kernel, PortabilityOutcome::Unknown,
             "the destination does not publish a kernel format capability", ReasonBasis::Derived,
             derived_precision, derived_class);
    } else if (supported) {
      record(PortabilityDimension::Kernel, PortabilityOutcome::PortableDirect,
             "the destination accepts kernel format " + inputs.artifact->kernel_format,
             ReasonBasis::Derived, derived_precision, derived_class);
    } else {
      record(PortabilityDimension::Kernel, PortabilityOutcome::PortableWithRecompile,
             "kernel format " + inputs.artifact->kernel_format +
                 " is not accepted by the destination; kernels must be recompiled",
             ReasonBasis::Derived, derived_precision, derived_class);
    }
  }

  // --- NumericalSemantic --------------------------------------------------
  if (inputs.workload == nullptr || inputs.workload->requirements.empty()) {
    record(PortabilityDimension::NumericalSemantic, PortabilityOutcome::Unknown,
           "the workload generation states no numerical requirements",
           ReasonBasis::Unattributed, Precision::Unknown, derived_class, false);
  } else if (inputs.destination_capabilities == nullptr) {
    record(PortabilityDimension::NumericalSemantic, PortabilityOutcome::Unknown,
           "the destination publishes no capability evidence", ReasonBasis::Unattributed,
           Precision::Unknown, derived_class);
  } else {
    PortabilityOutcome worst = PortabilityOutcome::PortableDirect;
    std::string detail = "every declared numerical requirement is satisfied";
    bool undecided = false;
    for (const CapabilityRequirement& requirement : inputs.workload->requirements) {
      const CapabilityCategory category = requirement.key.category();
      if (category != CapabilityCategory::PrecisionSupport) {
        continue;
      }
      const CapabilityCheck check = evaluate_requirement(requirement, *inputs.destination_capabilities);
      if (check.satisfied == Tri::No) {
        worst = weakest(worst, PortabilityOutcome::NotPortableArchitecture);
        detail = "the destination does not satisfy " + requirement.to_string();
      } else if (check.satisfied == Tri::Unknown) {
        undecided = true;
        detail = "the destination evidence does not decide " + requirement.to_string();
      }
    }
    if (undecided && worst == PortabilityOutcome::PortableDirect) {
      worst = PortabilityOutcome::Unknown;
    }
    record(PortabilityDimension::NumericalSemantic, worst, detail, ReasonBasis::Derived,
           derived_precision, derived_class);
  }

  // --- Performance --------------------------------------------------------
  record(PortabilityDimension::Performance, PortabilityOutcome::Unknown,
         "Federation Observatory does not measure workload performance and therefore never "
         "asserts performance portability",
         ReasonBasis::Unattributed, Precision::Unknown, derived_class, false);

  // --- Operational --------------------------------------------------------
  switch (inputs.destination_cluster->currentness) {
    case Currentness::Current:
      switch (inputs.destination_cluster->readiness) {
        case Readiness::Ready:
          record(PortabilityDimension::Operational, PortabilityOutcome::PortableDirect,
                 "the destination is current and reports ready", ReasonBasis::Observed,
                 derived_precision, derived_class);
          break;
        case Readiness::Draining:
          record(PortabilityDimension::Operational, PortabilityOutcome::NotPortableRuntime,
                 "the destination is draining and is not accepting new work",
                 ReasonBasis::Observed, derived_precision, derived_class);
          break;
        case Readiness::NotReady:
          record(PortabilityDimension::Operational, PortabilityOutcome::NotPortableRuntime,
                 "the destination reports not-ready", ReasonBasis::Observed, derived_precision,
                 derived_class);
          break;
        case Readiness::Retired:
          record(PortabilityDimension::Operational, PortabilityOutcome::NotPortableRuntime,
                 "the destination is retired", ReasonBasis::Observed, derived_precision,
                 derived_class);
          break;
        case Readiness::Unknown:
          record(PortabilityDimension::Operational, PortabilityOutcome::Unknown,
                 "the destination does not publish readiness", ReasonBasis::Observed,
                 derived_precision, derived_class);
          break;
      }
      break;
    case Currentness::RevalidationRequired:
      record(PortabilityDimension::Operational, PortabilityOutcome::Unknown,
             "destination evidence requires revalidation and cannot support an operational "
             "conclusion",
             ReasonBasis::Derived, Precision::Unknown, EvidenceClass::Unknown);
      break;
    default:
      record(PortabilityDimension::Operational, PortabilityOutcome::Unknown,
             std::string("destination evidence is ") +
                 std::string(fo::to_string(inputs.destination_cluster->currentness)),
             ReasonBasis::Derived, Precision::Unknown, EvidenceClass::Unknown);
      break;
  }

  // --- Policy -------------------------------------------------------------
  //
  // Policy is deliberately NOT one of the eleven technical dimensions. It is carried on
  // the assessment as its own flag and its own evidence entry so that a policy
  // restriction can never be read as a technical incompatibility, and vice versa.
  if (inputs.policy == nullptr) {
    assessment.policy_blocked = false;
    const Status s = assessment.evidence.add(
        Provenance::DerivedAnalysis, derived_class, "portability-assessor", Precision::Unknown,
        "no policy record was supplied; policy portability is not evaluated and NOT_PORTABLE_POLICY "
        "is not asserted");
    (void)s;
  } else {
    const Tri site_permitted = inputs.policy->permits_site(inputs.destination_cluster->site);
    const Tri cluster_permitted = inputs.policy->permits_cluster(inputs.destination_cluster->id);
    const Tri permitted = tri_and(site_permitted, cluster_permitted);
    assessment.policy_blocked = permitted == Tri::No;
    if (permitted == Tri::No) {
      const Status s = assessment.evidence.add(
          Provenance::DerivedAnalysis, derived_class, inputs.policy->id.value(), Precision::Derived,
          "policy generation " + inputs.policy->generation.to_string() +
              " does not permit destination " + inputs.destination_cluster->id.value() +
              "; this is a policy restriction, not a technical incompatibility");
      (void)s;
    } else if (permitted == Tri::Unknown) {
      const Status s = assessment.evidence.add(
          Provenance::DerivedAnalysis, derived_class, inputs.policy->id.value(), Precision::Unknown,
          "policy admissibility of the destination could not be decided");
      (void)s;
    }
  }

  // Aggregate.
  PortabilityOutcome overall = PortabilityOutcome::PortableDirect;
  Precision precision = derived_precision;
  EvidenceClass evidence_class = derived_class;
  bool first = true;
  for (const PortabilityDimensionResult& result : assessment.dimensions) {
    // Dimensions that nothing constrains are reported but do not decide the verdict.
    if (result.constrains_overall) {
      overall = first ? result.outcome : weakest(overall, result.outcome);
      precision = first ? result.precision : weakest(precision, result.precision);
      evidence_class = first ? result.evidence_class : weaker(evidence_class, result.evidence_class);
      first = false;
    }
    switch (result.outcome) {
      case PortabilityOutcome::PortableWithRebuild:
        assessment.requires_rebuild = true;
        break;
      case PortabilityOutcome::PortableWithRecompile:
        assessment.requires_recompile = true;
        break;
      case PortabilityOutcome::PortableWithConversion:
      case PortabilityOutcome::PortableWithStateTranslation:
        assessment.requires_conversion = true;
        break;
      case PortabilityOutcome::PortableWithFallback:
        assessment.requires_fallback = true;
        break;
      case PortabilityOutcome::NotPortableState:
        assessment.requires_state_translation = true;
        assessment.technically_blocked = true;
        break;
      default:
        if (is_technical_failure(result.outcome)) {
          assessment.technically_blocked = true;
        }
        break;
    }
  }
  if (first) {
    // Every dimension was unconstrained; nothing decided this verdict.
    overall = PortabilityOutcome::Unknown;
    precision = Precision::Unknown;
    evidence_class = EvidenceClass::Unknown;
    first = false;
  }
  if (assessment.policy_blocked) {
    overall = weakest(overall, PortabilityOutcome::NotPortablePolicy);
  }
  assessment.overall = overall;
  assessment.precision = precision;
  assessment.evidence_class = evidence_class;

  const Status s = assessment.evidence.add(
      Provenance::DerivedAnalysis, evidence_class, "portability-assessor", Precision::Derived,
      "evaluated " + std::to_string(assessment.dimensions.size()) + " dimensions for destination " +
          assessment.destination.value() + " (heterogeneous portability is never reduced to a "
          "single boolean)");
  (void)s;
  return assessment;
}

}  // namespace fo
