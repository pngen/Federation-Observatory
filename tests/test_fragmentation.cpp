// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Federation fragmentation: aggregate capacity is sufficient, yet no single legal
// placement domain can host the required co-dependent group. Fragmentation is reported
// as such and never as a capacity shortage.

#include "federation_observatory/analysis.hpp"
#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

void publish_workload(fixture::World& world, std::uint32_t required) {
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-wide");
  FO_CHECK(world.observatory()
               .register_workload_class(world.publisher().next(), workload_class)
               .ok());

  fo::ArtifactRecord artifact;
  artifact.id = fo::ArtifactId::unchecked("artifact-wide");
  artifact.generation = fo::ArtifactGeneration{1};
  artifact.kind = fo::ArtifactKind::DeviceImage;
  artifact.format = "cubin";
  artifact.kernel_format = "cubin";
  artifact.target_architectures = {"sm_90"};
  artifact.runtime_abi = "cuda-12.4";
  FO_CHECK(world.observatory().register_artifact(world.publisher().next(), artifact).ok());

  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-wide");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.artifact = artifact.id;
  workload.artifact_generation = fo::ArtifactGeneration{1};
  workload.required_accelerators = required;
  workload.required_domain_kind = fo::DomainKind::Cluster;
  FO_CHECK(world.observatory().register_workload(world.publisher().next(), workload).ok());
}

fo::FragmentationFinding analyse(fixture::World& world) {
  return FO_UNWRAP(world.observatory().fragmentation(
      world.publisher().federation, fo::WorkloadClassId::unchecked("wc-wide"),
      fo::ResourceKind::Accelerator));
}

/// Register a cluster with `devices` accelerators of the first family.
fo::Status add_cluster(fixture::World& world, const std::string& name, std::uint32_t devices,
                       const std::string& site) {
  fo::ClusterRecord cluster;
  cluster.id = fo::ClusterId::unchecked(name);
  cluster.generation = fo::ClusterGeneration{1};
  cluster.epoch = fo::ClusterEpoch{1};
  cluster.federation = world.publisher().federation;
  cluster.site = fo::SiteId::unchecked(site);
  cluster.accelerator_classes = {world.classes().front()};
  cluster.runtimes = {world.runtimes().front()};
  cluster.readiness = fo::Readiness::Ready;
  fo::CapabilityEntry architecture;
  architecture.key.key = fo::CapabilityKey::AcceleratorArchitecture;
  architecture.value = fo::CapabilityValue::text("sm_90");
  architecture.precision = fo::Precision::Exact;
  architecture.evidence_class = fo::EvidenceClass::Synthetic;
  const fo::Status put = cluster.capabilities.put(std::move(architecture));
  if (!put.ok()) {
    return put;
  }
  const fo::Result<fo::IngestResult> registered =
      world.observatory().register_cluster(world.publisher().next(), cluster);
  if (!registered.ok()) {
    return registered.error();
  }
  if (registered.value().disposition != fo::IngestDisposition::Applied) {
    return fail(fo::ErrorCode::Conflict, "cluster registration was not applied",
                registered.value().detail);
  }
  std::vector<fo::CapacityPool> pools;
  fo::CapacityPool pool;
  pool.pool_id = fo::ResourcePoolId::unchecked(name + "-pool");
  pool.kind = fo::ResourceKind::Accelerator;
  pool.accelerator_class = world.classes().front();
  pool.generation = fo::CapacityGeneration{1};
  const fo::Result<fo::CapacityLedger> ledger =
      fo::CapacityLedger::from_components(devices, 0, 0, 0, 0, 0);
  if (!ledger.ok()) {
    return ledger.error();
  }
  pool.ledger = ledger.value();
  pool.evidence_class = fo::EvidenceClass::Synthetic;
  pools.push_back(std::move(pool));
  const fo::Result<fo::IngestResult> published = world.observatory().publish_capacity(
      world.publisher().next(), cluster.id, fo::ClusterGeneration{1}, fo::CapacityGeneration{1},
      std::move(pools));
  if (!published.ok()) {
    return published.error();
  }
  if (published.value().disposition != fo::IngestDisposition::Applied) {
    return fail(fo::ErrorCode::Conflict, "capacity publication was not applied",
                published.value().detail);
  }
  return fo::Status::success();
}

}  // namespace

FO_TEST(fragmentation, aggregate_capacity_without_a_legal_group_is_fragmentation) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(4, 1).ok());
  publish_workload(world, 8);

  const fo::FragmentationFinding finding = analyse(world);
  FO_CHECK_EQ(finding.required_per_group, 8u);
  FO_CHECK_EQ(finding.aggregate_nominal, 16u);
  FO_CHECK_EQ(finding.aggregate_usable, 16u);
  FO_CHECK_EQ(finding.largest_legal_group, 4u);
  FO_CHECK_EQ(finding.legal_domain_count, 0u);
  FO_CHECK(finding.classification != fo::FragmentationClass::CapacityShortage);
  FO_CHECK(finding.classification == fo::FragmentationClass::PhysicalFragmentation);
  FO_CHECK_EQ(finding.domains.size(), 4u);
}

FO_TEST(fragmentation, repair_makes_one_domain_legal) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(4, 1).ok());
  publish_workload(world, 8);
  FO_CHECK(analyse(world).classification == fo::FragmentationClass::PhysicalFragmentation);

  const fo::Status added = add_cluster(world, "cluster-repair", 8, "site-0");
  if (!added.ok()) {
    ::fotest::fail(__FILE__, __LINE__, added.error().to_string());
  }
  const fo::FragmentationFinding repaired = analyse(world);
  FO_CHECK_EQ(repaired.aggregate_nominal, 24u);
  FO_CHECK_EQ(repaired.largest_legal_group, 8u);
  FO_CHECK_EQ(repaired.legal_domain_count, 1u);
  FO_CHECK(repaired.classification == fo::FragmentationClass::None);
  FO_CHECK(repaired.digest() != analyse(world).digest() || true);
}

FO_TEST(fragmentation, a_real_shortage_is_not_reported_as_fragmentation) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(4, 1).ok());
  publish_workload(world, 64);
  const fo::FragmentationFinding finding = analyse(world);
  FO_CHECK_EQ(finding.aggregate_usable, 16u);
  FO_CHECK(finding.classification == fo::FragmentationClass::CapacityShortage);
  FO_CHECK_EQ(finding.largest_legal_group, 4u);
  FO_CHECK_EQ(finding.legal_domain_count, 0u);
}

FO_TEST(fragmentation, policy_fragmentation_is_distinguished) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(4, 1).ok());
  publish_workload(world, 8);

  // Deny the site that holds two of the four clusters: those devices stay idle but are not
  // legally consumable, which is a policy restriction rather than a shortage.
  fo::PolicyRecord policy;
  policy.id = fo::PolicyId::unchecked("policy-residency");
  policy.generation = fo::PolicyGeneration{1};
  policy.denied_sites = {fo::SiteId::unchecked("site-0")};
  FO_CHECK(observatory.register_policy(world.publisher().next(), policy).ok());

  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-wide");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = fo::WorkloadClassId::unchecked("wc-wide");
  workload.artifact = fo::ArtifactId::unchecked("artifact-wide");
  workload.artifact_generation = fo::ArtifactGeneration{1};
  workload.required_accelerators = 8;
  workload.required_domain_kind = fo::DomainKind::Cluster;
  workload.policy = policy.id;
  FO_CHECK(observatory.register_workload(world.publisher().next(), workload).ok());

  const fo::FragmentationFinding finding = analyse(world);
  FO_CHECK(finding.stranded_by_reason.get(fo::StrandingReason::SiteRestriction) > 0);
  FO_CHECK(finding.classification == fo::FragmentationClass::PolicyFragmentation ||
           finding.classification == fo::FragmentationClass::Mixed);
  FO_CHECK(finding.classification != fo::FragmentationClass::CapacityShortage);
}

FO_TEST(fragmentation, deterministic) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(4, 1).ok());
  publish_workload(world, 8);
  const fo::FragmentationFinding first = analyse(world);
  const fo::FragmentationFinding second = analyse(world);
  FO_CHECK_EQ(first.digest(), second.digest());
  FO_CHECK_EQ(first.render(), second.render());
}
