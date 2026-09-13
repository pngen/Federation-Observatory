// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Portability is per dimension. Direct, rebuild, recompile, conversion, state
// translation, policy block, architecture failure and unknown are all distinct.

#include "federation_observatory/portability.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

/// A destination that is directly portable unless a test changes one fact about it.
struct Scenario {
  WorkloadRecord workload;
  ArtifactRecord artifact;
  ClusterRecord source;
  ClusterRecord destination;
  AcceleratorClassRecord source_class;
  AcceleratorClassRecord destination_class;
  RuntimeRecord source_runtime;
  RuntimeRecord destination_runtime;
  PolicyRecord policy;
  CapabilitySet destination_capabilities;
  bool use_policy = false;

  Scenario() {
    workload.id = WorkloadId::unchecked("wl-1");
    workload.generation = WorkloadGeneration{1};
    workload.workload_class = WorkloadClassId::unchecked("wc-1");
    workload.artifact = ArtifactId::unchecked("artifact-1");
    workload.artifact_generation = ArtifactGeneration{1};
    workload.required_accelerators = 2;
    workload.required_memory_bytes_per_accelerator = 24 * kGiB;

    artifact.id = ArtifactId::unchecked("artifact-1");
    artifact.generation = ArtifactGeneration{1};
    artifact.kind = ArtifactKind::DeviceImage;
    artifact.format = "cubin";
    artifact.kernel_format = "cubin";
    artifact.target_architectures = {"sm_90"};
    artifact.minimum_compute_capability = "9.0";
    artifact.runtime_abi = "cuda-12.4";
    artifact.driver_abi = "550.54";
    artifact.compiler_target = "sm_90";
    artifact.state_format = "torch-distributed-v2";

    source.id = ClusterId::unchecked("cluster-a");
    source.generation = ClusterGeneration{1};
    source.epoch = ClusterEpoch{1};
    source.federation = FederationId::unchecked("fed-1");
    source.site = SiteId::unchecked("site-a");
    source.readiness = Readiness::Ready;
    source.currentness = Currentness::Current;

    destination.id = ClusterId::unchecked("cluster-b");
    destination.generation = ClusterGeneration{1};
    destination.epoch = ClusterEpoch{1};
    destination.federation = FederationId::unchecked("fed-1");
    destination.site = SiteId::unchecked("site-b");
    destination.readiness = Readiness::Ready;
    destination.currentness = Currentness::Current;

    source_class.id = AcceleratorClassId::unchecked("accel-a");
    source_class.capability_generation = AcceleratorCapabilityGeneration{1};
    source_class.architecture = "sm_90";
    source_class.memory_bytes_per_device = 80 * kGiB;

    destination_class.id = AcceleratorClassId::unchecked("accel-b");
    destination_class.capability_generation = AcceleratorCapabilityGeneration{1};
    destination_class.architecture = "sm_90";
    destination_class.memory_bytes_per_device = 80 * kGiB;

    source_runtime.id = RuntimeId::unchecked("runtime-cuda-12.4");
    source_runtime.generation = RuntimeGeneration{1};
    source_runtime.kind = RuntimeKind::Cuda;
    source_runtime.abi = "cuda-12.4";
    source_runtime.driver_abi = "550.54";

    destination_runtime.id = RuntimeId::unchecked("runtime-cuda-12.4");
    destination_runtime.generation = RuntimeGeneration{1};
    destination_runtime.kind = RuntimeKind::Cuda;
    destination_runtime.abi = "cuda-12.4";
    destination_runtime.driver_abi = "550.54";

    policy.id = PolicyId::unchecked("policy-1");
    policy.generation = PolicyGeneration{1};

    publish(CapabilityKey::ArtifactFormats,
            FO_UNWRAP(CapabilityValue::text_set({"cubin", "elf", "torch-distributed-v2"})));
    publish(CapabilityKey::KernelFormat, CapabilityValue::text("cubin"));
    publish(CapabilityKey::CompilerTarget, CapabilityValue::text("sm_90"));
    publish(CapabilityKey::InferenceServingBackend,
            FO_UNWRAP(CapabilityValue::text_set({"text-generation-inference"})));
  }

  void publish(CapabilityKey key, CapabilityValue value) {
    CapabilityEntry entry;
    entry.key.key = key;
    entry.value = std::move(value);
    const Status status = destination_capabilities.put(std::move(entry));
    FO_REQUIRE(status.ok());
  }

  [[nodiscard]] PortabilityAssessment assess() {
    PortabilityInputs inputs;
    inputs.workload = &workload;
    inputs.artifact = &artifact;
    inputs.source_cluster = &source;
    inputs.source_accelerator_class = &source_class;
    inputs.source_runtime = &source_runtime;
    inputs.destination_cluster = &destination;
    inputs.destination_accelerator_class = &destination_class;
    inputs.destination_runtime = &destination_runtime;
    inputs.destination_capabilities = &destination_capabilities;
    inputs.policy = use_policy ? &policy : nullptr;
    return assess_portability(inputs);
  }
};

PortabilityOutcome dimension_of(const PortabilityAssessment& assessment,
                                PortabilityDimension dimension) {
  const PortabilityDimensionResult* result = assessment.find(dimension);
  FO_REQUIRE(result != nullptr);
  return result->outcome;
}

}  // namespace

FO_TEST(portability, every_dimension_is_reported) {
  Scenario scenario;
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK_EQ(assessment.dimensions.size(), kPortabilityDimensionCount);
  FO_CHECK(assessment.validate().ok());
  FO_CHECK(assessment.overall == PortabilityOutcome::PortableDirect);
  FO_CHECK(!assessment.technically_blocked);
  FO_CHECK(!assessment.policy_blocked);
}

FO_TEST(portability, architecture_mismatch_without_a_compiler_target_is_a_technical_failure) {
  Scenario scenario;
  scenario.destination_class.architecture = "sm_80";
  scenario.artifact.compiler_target.clear();
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Binary) ==
           PortabilityOutcome::NotPortableArchitecture);
  FO_CHECK(is_technical_failure(assessment.overall));
  FO_CHECK(!is_policy_failure(assessment.overall));
  FO_CHECK(assessment.technically_blocked);
}

FO_TEST(portability, architecture_mismatch_with_a_destination_compiler_target_requires_a_rebuild) {
  Scenario scenario;
  scenario.destination_class.architecture = "sm_89";
  scenario.publish(CapabilityKey::CompilerTarget, CapabilityValue::text("sm_90"));
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Binary) ==
           PortabilityOutcome::PortableWithRebuild);
  FO_CHECK(assessment.requires_rebuild);
  FO_CHECK(!assessment.technically_blocked);
}

FO_TEST(portability, runtime_family_change_is_not_a_rebuild) {
  Scenario scenario;
  scenario.destination_runtime.kind = RuntimeKind::Rocm;
  scenario.destination_runtime.abi = "rocm-6.2";
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::RuntimeApi) ==
           PortabilityOutcome::NotPortableRuntime);
}

FO_TEST(portability, runtime_generation_change_requires_a_recompile) {
  Scenario scenario;
  scenario.destination_runtime.abi = "cuda-13.0";
  scenario.destination_runtime.generation = RuntimeGeneration{2};
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::RuntimeApi) ==
           PortabilityOutcome::PortableWithRecompile);
  FO_CHECK(assessment.requires_recompile);
}

FO_TEST(portability, unsupported_checkpoint_format_requires_state_translation) {
  Scenario scenario;
  scenario.publish(CapabilityKey::ArtifactFormats,
                   FO_UNWRAP(CapabilityValue::text_set({"cubin", "elf"})));
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Checkpoint) ==
           PortabilityOutcome::PortableWithStateTranslation);
  FO_CHECK(dimension_of(assessment, PortabilityDimension::State) ==
           PortabilityOutcome::NotPortableState);
  FO_CHECK(assessment.requires_state_translation);
  FO_CHECK(assessment.technically_blocked);
}

FO_TEST(portability, kernel_format_mismatch_requires_a_recompile) {
  Scenario scenario;
  scenario.publish(CapabilityKey::KernelFormat, CapabilityValue::text("hsaco"));
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Kernel) ==
           PortabilityOutcome::PortableWithRecompile);
}

FO_TEST(portability, missing_artifact_format_evidence_is_unknown) {
  Scenario scenario;
  scenario.destination_capabilities = CapabilitySet{};
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Artifact) ==
           PortabilityOutcome::Unknown);
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Kernel) == PortabilityOutcome::Unknown);
  FO_CHECK(is_undecided(assessment.overall));
}

FO_TEST(portability, policy_block_is_not_a_technical_failure) {
  Scenario scenario;
  scenario.use_policy = true;
  scenario.policy.denied_sites = {scenario.destination.site};
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(assessment.policy_blocked);
  FO_CHECK(is_policy_failure(assessment.overall));
  FO_CHECK(!assessment.technically_blocked);
  FO_CHECK(!is_technical_failure(assessment.overall));
}

FO_TEST(portability, performance_portability_is_never_asserted) {
  Scenario scenario;
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Performance) ==
           PortabilityOutcome::Unknown);
}

FO_TEST(portability, destination_not_ready_is_operational) {
  Scenario scenario;
  scenario.destination.readiness = Readiness::Draining;
  const PortabilityAssessment assessment = scenario.assess();
  FO_CHECK(dimension_of(assessment, PortabilityDimension::Operational) ==
           PortabilityOutcome::NotPortableRuntime);

  Scenario stale;
  stale.destination.currentness = Currentness::RevalidationRequired;
  const PortabilityAssessment stale_assessment = stale.assess();
  FO_CHECK(dimension_of(stale_assessment, PortabilityDimension::Operational) ==
           PortabilityOutcome::Unknown);
}

FO_TEST(portability, missing_destination_is_reported_as_unknown_not_portable) {
  PortabilityInputs inputs;
  WorkloadRecord workload;
  workload.id = WorkloadId::unchecked("wl-1");
  workload.generation = WorkloadGeneration{1};
  inputs.workload = &workload;
  const PortabilityAssessment assessment = assess_portability(inputs);
  FO_CHECK(assessment.overall == PortabilityOutcome::Unknown);
  FO_CHECK_EQ(assessment.dimensions.size(), kPortabilityDimensionCount);
  FO_CHECK(is_undecided(assessment.overall));
}

FO_TEST(portability, outcome_classification_helpers) {
  FO_CHECK(requires_adaptation(PortabilityOutcome::PortableWithRebuild));
  FO_CHECK(!requires_adaptation(PortabilityOutcome::PortableDirect));
  FO_CHECK(is_technical_failure(PortabilityOutcome::NotPortableArchitecture));
  FO_CHECK(!is_technical_failure(PortabilityOutcome::NotPortablePolicy));
  FO_CHECK(is_policy_failure(PortabilityOutcome::NotPortablePolicy));
  FO_CHECK(is_undecided(PortabilityOutcome::Unknown));
  FO_CHECK(portability_severity(PortabilityOutcome::NotPortableArchitecture) >
           portability_severity(PortabilityOutcome::PortableWithRebuild));
  FO_CHECK(weakest(PortabilityOutcome::PortableDirect, PortabilityOutcome::NotPortableState) ==
           PortabilityOutcome::NotPortableState);
  FO_CHECK(weakest(PortabilityOutcome::NotPortablePolicy, PortabilityOutcome::Unknown) ==
           PortabilityOutcome::Unknown);
}
