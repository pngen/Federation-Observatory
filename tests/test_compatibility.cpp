// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Named compatibility outcomes: every relationship is explained, nothing is scored.

#include "federation_observatory/compatibility.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

struct Scenario {
  WorkloadRecord workload;
  ArtifactRecord artifact;
  ClusterRecord cluster;
  AcceleratorClassRecord accelerator_class;
  RuntimeRecord runtime;
  CapabilitySet capabilities;

  Scenario() {
    workload.id = WorkloadId::unchecked("wl-1");
    workload.generation = WorkloadGeneration{1};
    workload.workload_class = WorkloadClassId::unchecked("wc-1");
    workload.artifact = ArtifactId::unchecked("artifact-1");
    workload.artifact_generation = ArtifactGeneration{1};
    workload.required_accelerators = 8;
    workload.required_memory_bytes_per_accelerator = 64 * kGiB;
    workload.stamp.precision = Precision::Exact;
    workload.stamp.evidence_class = EvidenceClass::Synthetic;

    artifact.id = ArtifactId::unchecked("artifact-1");
    artifact.generation = ArtifactGeneration{1};
    artifact.kind = ArtifactKind::DeviceImage;
    artifact.format = "cubin";
    artifact.kernel_format = "cubin";
    artifact.target_architectures = {"sm_90"};
    artifact.minimum_compute_capability = "9.0";
    artifact.runtime_abi = "cuda-12.4";
    artifact.driver_abi = "550.54";
    artifact.stamp.precision = Precision::Exact;
    artifact.stamp.evidence_class = EvidenceClass::Synthetic;

    cluster.id = ClusterId::unchecked("cluster-1");
    cluster.generation = ClusterGeneration{1};
    cluster.epoch = ClusterEpoch{1};
    cluster.federation = FederationId::unchecked("fed-1");
    cluster.site = SiteId::unchecked("site-1");
    cluster.readiness = Readiness::Ready;
    cluster.currentness = Currentness::Current;
    cluster.stamp.precision = Precision::Exact;
    cluster.stamp.evidence_class = EvidenceClass::Synthetic;

    accelerator_class.id = AcceleratorClassId::unchecked("accel-1");
    accelerator_class.capability_generation = AcceleratorCapabilityGeneration{1};
    accelerator_class.architecture = "sm_90";
    accelerator_class.compute_capability = "9.0";
    accelerator_class.memory_bytes_per_device = 80 * kGiB;
    accelerator_class.stamp.precision = Precision::Exact;
    accelerator_class.stamp.evidence_class = EvidenceClass::Synthetic;

    runtime.id = RuntimeId::unchecked("runtime-1");
    runtime.generation = RuntimeGeneration{1};
    runtime.kind = RuntimeKind::Cuda;
    runtime.abi = "cuda-12.4";
    runtime.driver_abi = "550.54";
    runtime.stamp.precision = Precision::Exact;
    runtime.stamp.evidence_class = EvidenceClass::Synthetic;

    publish(CapabilityKey::RuntimeAbi, CapabilityValue::text("cuda-12.4"));
    publish(CapabilityKey::KernelFormat, CapabilityValue::text("cubin"));
    const Result<CapabilityValue> formats = CapabilityValue::text_set({"cubin", "fatbin"});
    publish(CapabilityKey::ArtifactFormats, formats.value());
    publish(CapabilityKey::CompilerTarget, CapabilityValue::text("sm_90"));
  }

  void publish(CapabilityKey key, CapabilityValue value) {
    CapabilityEntry cap_entry;
    cap_entry.key.key = key;
    cap_entry.value = std::move(value);
    cap_entry.precision = Precision::Exact;
    cap_entry.evidence_class = EvidenceClass::Synthetic;
    const Status status = capabilities.put(std::move(cap_entry));
    FO_REQUIRE(status.ok());
  }

  [[nodiscard]] CompatibilityAssessment assess() const {
    CompatibilityInputs inputs;
    inputs.workload = &workload;
    inputs.artifact = &artifact;
    inputs.cluster = &cluster;
    inputs.accelerator_class = &accelerator_class;
    inputs.cluster_runtime = &runtime;
    inputs.effective_capabilities = &capabilities;
    inputs.target_currentness = cluster.currentness;
    inputs.compatibility_generation = CompatibilityGeneration{1};
    return assess_compatibility(inputs);
  }
};

}  // namespace

FO_TEST(compatibility, fully_compatible_target) {
  const Scenario scenario;
  const CompatibilityAssessment assessment = scenario.assess();
  FO_CHECK(assessment.overall == CompatibilityOutcome::Compatible);
  FO_CHECK(assessment.admits_placement());
  FO_CHECK(!assessment.requires_adaptation());
  FO_CHECK(assessment.primary_failure() == nullptr);
  FO_CHECK(!assessment.checks.empty());
}

FO_TEST(compatibility, architecture_mismatch_is_named) {
  Scenario scenario;
  scenario.artifact.target_architectures = {"sm_80"};
  const CompatibilityAssessment assessment = scenario.assess();
  FO_CHECK(assessment.overall == CompatibilityOutcome::IncompatibleArchitecture);
  FO_CHECK(is_hard_incompatible(assessment.overall));
  FO_CHECK(!assessment.admits_placement());
  const CompatibilityCheck* failure = assessment.primary_failure();
  FO_REQUIRE(failure != nullptr);
  FO_CHECK(failure->subject == CompatibilitySubject::ArtifactArchitecture);
}

FO_TEST(compatibility, missing_target_architecture_is_unknown_not_incompatible) {
  Scenario scenario;
  scenario.accelerator_class.architecture.clear();
  const CompatibilityAssessment assessment = scenario.assess();
  FO_CHECK(is_undecided(assessment.overall));
  FO_CHECK(assessment.overall == CompatibilityOutcome::UnknownCompatibility);
  FO_CHECK(!assessment.admits_placement());
}

FO_TEST(compatibility, runtime_and_driver_abi_mismatch) {
  Scenario scenario;
  scenario.runtime.abi = "cuda-13.0";
  const CompatibilityAssessment assessment = scenario.assess();
  FO_CHECK(assessment.overall == CompatibilityOutcome::IncompatibleAbi);

  Scenario driver;
  driver.runtime.driver_abi = "560.28";
  const CompatibilityAssessment driver_assessment = driver.assess();
  FO_CHECK(driver_assessment.overall == CompatibilityOutcome::IncompatibleDriver);
}

FO_TEST(compatibility, memory_requirement) {
  Scenario too_small;
  too_small.workload.required_memory_bytes_per_accelerator = 128 * kGiB;
  const CompatibilityAssessment assessment = too_small.assess();
  FO_CHECK(assessment.overall == CompatibilityOutcome::IncompatibleMemory);

  Scenario unknown;
  unknown.accelerator_class.memory_bytes_per_device = 0;
  FO_CHECK(is_undecided(unknown.assess().overall));

  // A workload that states no memory requirement does not constrain memory, so no memory
  // check is emitted and the assessment stays decidable.
  Scenario no_requirement;
  no_requirement.workload.required_memory_bytes_per_accelerator = 0;
  FO_CHECK(no_requirement.assess().overall == CompatibilityOutcome::Compatible);
}

FO_TEST(compatibility, stale_evidence_is_not_compatibility) {
  Scenario scenario;
  scenario.cluster.currentness = Currentness::Stale;
  const CompatibilityAssessment assessment = scenario.assess();
  FO_CHECK(assessment.overall == CompatibilityOutcome::StaleEvidence);
  FO_CHECK(is_undecided(assessment.overall));

  Scenario revalidation;
  revalidation.cluster.currentness = Currentness::RevalidationRequired;
  FO_CHECK(revalidation.assess().overall == CompatibilityOutcome::StaleEvidence);

  Scenario retired;
  retired.cluster.currentness = Currentness::Retired;
  FO_CHECK(retired.assess().overall == CompatibilityOutcome::Unsupported);
}

FO_TEST(compatibility, policy_is_tracked_outside_the_verdict) {
  Scenario scenario;
  PolicyRecord policy;
  policy.id = PolicyId::unchecked("policy-1");
  policy.generation = PolicyGeneration{1};
  policy.denied_sites = {scenario.cluster.site};
  scenario.workload.policy = policy.id;
  CompatibilityInputs inputs;
  inputs.workload = &scenario.workload;
  inputs.artifact = &scenario.artifact;
  inputs.cluster = &scenario.cluster;
  inputs.accelerator_class = &scenario.accelerator_class;
  inputs.cluster_runtime = &scenario.runtime;
  inputs.effective_capabilities = &scenario.capabilities;
  inputs.policy = &policy;
  inputs.target_currentness = Currentness::Current;
  const CompatibilityAssessment assessment = assess_compatibility(inputs);
  // Technically perfectly compatible, administratively inadmissible.
  FO_CHECK(assessment.overall == CompatibilityOutcome::Compatible);
  FO_CHECK(assessment.policy_admissible == Tri::No);
  FO_CHECK(!assessment.admits_placement());
  FO_CHECK(assessment.policy_detail.find("does not permit") != std::string::npos);
}

FO_TEST(compatibility, severity_ordering_is_total) {
  FO_CHECK(compatibility_severity(CompatibilityOutcome::Compatible) <
           compatibility_severity(CompatibilityOutcome::CompatibleWithFallback));
  FO_CHECK(compatibility_severity(CompatibilityOutcome::IncompatibleArchitecture) <
           compatibility_severity(CompatibilityOutcome::UnknownCompatibility));
  FO_CHECK(weakest(CompatibilityOutcome::Compatible, CompatibilityOutcome::IncompatibleRuntime) ==
           CompatibilityOutcome::IncompatibleRuntime);
  FO_CHECK(weakest(CompatibilityOutcome::IncompatibleRuntime,
                   CompatibilityOutcome::UnknownCompatibility) ==
           CompatibilityOutcome::UnknownCompatibility);
}

FO_TEST(compatibility, runtime_version_comparison_helper) {
  FO_CHECK(compare_runtime_versions("cuda-12.4", "cuda-12.4", "", "") ==
           CompatibilityOutcome::Compatible);
  FO_CHECK(compare_runtime_versions("cuda-12.4", "cuda-13.0", "", "") ==
           CompatibilityOutcome::IncompatibleAbi);
  FO_CHECK(compare_runtime_versions("cuda-12.4", "", "", "") ==
           CompatibilityOutcome::UnknownCompatibility);
  FO_CHECK(compare_runtime_versions("", "", "550.54", "560.28") ==
           CompatibilityOutcome::IncompatibleDriver);
}
