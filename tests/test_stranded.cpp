// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Stranded-capacity proof. Nominal, idle, usable, stranded and unknown are computed with
// exact checked accounting, decomposed by reason, and recomputed when a generation
// changes so that a prior finding provably stops being current.

#include "federation_observatory/analysis.hpp"
#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

/// Three clusters: two of family-a (sm_90 with fp8) and one of family-b (gfx942 without
/// fp8). Four devices each, so the aggregate is 12 while only 8 are usable.
void publish_workload(fixture::World& world) {
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-train");
  workload_class.display_name = "training";
  FO_CHECK(world.observatory().register_workload_class(world.publisher().next(), workload_class).ok());

  fo::ArtifactRecord artifact;
  artifact.id = fo::ArtifactId::unchecked("artifact-train");
  artifact.generation = fo::ArtifactGeneration{1};
  artifact.kind = fo::ArtifactKind::DeviceImage;
  artifact.format = "cubin";
  artifact.kernel_format = "cubin";
  artifact.target_architectures = {"sm_90"};
  artifact.minimum_compute_capability = "9.0";
  artifact.runtime_abi = "cuda-12.4";
  artifact.required_memory_bytes = 64 * kGiB;
  FO_CHECK(world.observatory().register_artifact(world.publisher().next(), artifact).ok());

  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-train");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.artifact = artifact.id;
  workload.artifact_generation = fo::ArtifactGeneration{1};
  workload.required_accelerators = 8;
  workload.required_memory_bytes_per_accelerator = 64 * kGiB;
  workload.acceptable_accelerator_classes = {world.classes().front()};
  fo::CapabilityRequirement fp8;
  fp8.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  fp8.comparator = fo::CapabilityComparator::Present;
  fp8.rationale = "training requires fp8";
  workload.requirements.push_back(fp8);
  FO_CHECK(world.observatory().register_workload(world.publisher().next(), workload).ok());
}

fo::StrandedCapacityReport stranded(fixture::World& world) {
  fo::StrandedCapacityRequest request;
  request.federation = world.publisher().federation;
  request.workload_class = fo::WorkloadClassId::unchecked("wc-train");
  request.kind = fo::ResourceKind::Accelerator;
  return FO_UNWRAP(world.observatory().stranded_capacity(request));
}

}  // namespace

FO_TEST(stranded, accounting_closes_and_matches_the_expected_decomposition) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(3, 2).ok());
  publish_workload(world);

  const fo::StrandedCapacityReport report = stranded(world);
  FO_CHECK(report.closes());
  FO_CHECK_EQ(report.summary.nominal, 12u);
  FO_CHECK_EQ(report.summary.idle, 12u);
  FO_CHECK_EQ(report.summary.usable, 8u);
  FO_CHECK_EQ(report.summary.stranded, 4u);
  FO_CHECK_EQ(report.summary.unknown, 0u);
  FO_CHECK_EQ(report.summary.usable + report.summary.stranded + report.summary.unknown,
              report.summary.idle);
  FO_CHECK_EQ(percent_string(report.summary.stranded, report.summary.nominal),
              std::string("33.33%"));

  // Exactly one finding (the family-b cluster) carries stranded capacity, and it names a
  // technical reason rather than a generic "unavailable".
  std::uint64_t stranded_findings = 0;
  for (const fo::StrandedCapacityFinding& finding : report.findings) {
    if (finding.breakdown.stranded() != 0) {
      ++stranded_findings;
      FO_CHECK_EQ(finding.breakdown.stranded(), 4u);
      FO_CHECK(is_technical_reason(finding.breakdown.stranded_by_primary_reason.non_zero().front().first));
      FO_CHECK(finding.breakdown.stranded_by_primary_reason.get(
                   fo::StrandingReason::UnsupportedPrecision) == 4u);
    }
  }
  FO_CHECK_EQ(stranded_findings, 1u);
}

FO_TEST(stranded, deterministic_inputs_produce_an_identical_report) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(3, 2).ok());
  publish_workload(world);
  const fo::StrandedCapacityReport first = stranded(world);
  const fo::StrandedCapacityReport second = stranded(world);
  FO_CHECK_EQ(first.digest(), second.digest());
  FO_CHECK_EQ(first.render(), second.render());
}

FO_TEST(stranded, withdrawing_a_capability_generation_changes_the_conclusion) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(3, 2).ok());
  publish_workload(world);
  const fo::StrandedCapacityReport before = stranded(world);
  FO_CHECK_EQ(before.summary.usable, 8u);

  // The first cluster withdraws fp8 support under a new capability generation. This is
  // exactly the change that must invalidate the previous conclusion instead of leaving it
  // silently standing.
  // A capability is withdrawn explicitly, by publishing it false. Omitting the key would
  // leave the accelerator-class catalogue's declaration standing, which is the correct
  // union semantics: a capability is available if either the fleet or the device
  // catalogue declares it.
  fo::CapabilitySet capabilities;
  fo::CapabilityEntry architecture;
  architecture.key.key = fo::CapabilityKey::AcceleratorArchitecture;
  architecture.value = fo::CapabilityValue::text("sm_90");
  architecture.precision = fo::Precision::Exact;
  architecture.evidence_class = fo::EvidenceClass::Synthetic;
  FO_CHECK(capabilities.put(std::move(architecture)).ok());
  fo::CapabilityEntry fp8;
  fp8.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  fp8.value = fo::CapabilityValue::boolean(false);
  fp8.precision = fo::Precision::Exact;
  fp8.evidence_class = fo::EvidenceClass::Synthetic;
  FO_CHECK(capabilities.put(std::move(fp8)).ok());

  const fo::Result<fo::IngestResult> published = observatory.publish_capability(
      world.publisher().next(), world.clusters().front(), fo::ClusterGeneration{1},
      fo::AcceleratorCapabilityGeneration{2}, std::move(capabilities));
  FO_REQUIRE(published.ok());
  FO_CHECK(published.value().disposition == fo::IngestDisposition::Applied);

  const fo::StrandedCapacityReport after = stranded(world);
  FO_CHECK(after.closes());
  FO_CHECK_EQ(after.summary.usable, 4u);
  FO_CHECK_EQ(after.summary.stranded, 8u);
  FO_CHECK(after.digest() != before.digest());
  FO_CHECK(after.render() != before.render());
}

FO_TEST(stranded, stale_cluster_evidence_is_excluded_rather_than_counted) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());
  publish_workload(world);
  const fo::StrandedCapacityReport before = stranded(world);
  FO_CHECK_EQ(before.summary.nominal, 8u);

  // Fencing the publisher makes every record it produced non-current. The capacity of
  // those clusters must then be reported as excluded, not silently counted.
  FO_CHECK(observatory
                .fence_publisher(world.publisher().id, world.publisher().boot, "test fence")
                .ok());
  const fo::StrandedCapacityReport after = stranded(world);
  FO_CHECK_EQ(after.summary.nominal, 0u);
  FO_CHECK_EQ(after.excluded_stale_clusters.size(), 2u);
  FO_CHECK(after.findings.empty());
  FO_CHECK(!after.closes() || after.summary.idle == 0u);
}

FO_TEST(stranded, unknown_evidence_is_a_first_class_outcome) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(1, 1).ok());
  publish_workload(world);

  // Neither the cluster nor the accelerator class publishes an architecture, while the
  // artifact declares the architectures it targets. There is nothing to compare, so the
  // analysis must say UNKNOWN rather than calling the capacity usable or stranded.
  fo::CapabilitySet silent;
  fo::CapabilityEntry fp8;
  fp8.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  fp8.value = fo::CapabilityValue::boolean(true);
  fp8.precision = fo::Precision::Exact;
  fp8.evidence_class = fo::EvidenceClass::Synthetic;
  FO_CHECK(silent.put(std::move(fp8)).ok());
  fixture::require_applied(
      observatory.publish_capability(world.publisher().next(), world.clusters().front(),
                                     fo::ClusterGeneration{1},
                                     fo::AcceleratorCapabilityGeneration{2}, silent),
      "capability withdrawal");

  fo::AcceleratorClassRecord silent_class;
  silent_class.id = world.classes().front();
  silent_class.capability_generation = fo::AcceleratorCapabilityGeneration{2};
  silent_class.memory_bytes_per_device = 80 * fixture::kGiB;
  fo::CapabilityEntry memory;
  memory.key.key = fo::CapabilityKey::MemoryBytes;
  memory.value = fo::CapabilityValue::unsigned_integer(80 * fixture::kGiB);
  FO_CHECK(silent_class.capabilities.put(std::move(memory)).ok());
  fixture::require_applied(
      observatory.register_accelerator_class(world.publisher().next(), silent_class),
      "silent accelerator class");

  const fo::StrandedCapacityReport report = stranded(world);
  FO_CHECK_EQ(report.summary.idle, 4u);
  FO_CHECK_EQ(report.summary.unknown, 4u);
  FO_CHECK_EQ(report.summary.usable, 0u);
  FO_CHECK_EQ(report.summary.stranded, 0u);
  FO_CHECK_EQ(report.findings.size(), 1u);
  FO_CHECK_EQ(report.findings.front().breakdown.unknown, 4u);
}

FO_TEST(stranded, ledger_identity_is_enforced) {
  const fo::Result<fo::CapacityLedger> impossible =
      fo::CapacityLedger::from_components(4, 0, 3, 3, 0, 0);
  FO_CHECK(!impossible.ok());
  FO_CHECK(impossible.error().code() == fo::ErrorCode::CapacityInconsistent);

  fo::CapacityLedger tampered;
  tampered.nominal = 4;
  tampered.idle = 3;
  FO_CHECK(!tampered.validate().ok());

  const fo::Result<fo::CapacityLedger> valid =
      fo::CapacityLedger::from_components(10, 1, 2, 1, 1, 1);
  FO_REQUIRE(valid.ok());
  FO_CHECK_EQ(valid.value().idle, 4u);
  FO_CHECK_EQ(valid.value().online(), 9u);
  FO_CHECK_EQ(valid.value().committed(), 4u);
  FO_CHECK(valid.value().validate().ok());
}
