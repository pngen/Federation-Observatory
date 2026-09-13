// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_installed_consumer - a downstream consumer of the installed public API.
//
// What this proves: version_string(), build_banner(), Bounds and FederationObservatory are
// enough on their own to publish a small federation - federation, site, cluster,
// accelerator class, runtime, artifact, workload class, workload, capacity and a placement
// - and to read the resulting placement explanation back.
//
// This example deliberately does NOT include federation_observatory/synthetic.hpp: every
// record below is constructed by the caller and is the consumer's own claim, attributed to
// the consumer's publisher identity. Federation Observatory observes them and chooses
// nothing.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/capacity.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/version.hpp"

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

int fail(const fo::Status& status, const char* stage) {
  std::printf("FAILED [%s]: %s\n", stage, status.to_string().c_str());
  return 1;
}

template <class T>
int fail(const fo::Result<T>& result, const char* stage) {
  std::printf("FAILED [%s]: %s\n", stage, result.error().to_string().c_str());
  return 1;
}

void section(const char* title) { std::printf("\n== %s ==\n", title); }

/// The consumer's own publisher identity and monotonic sequence counter.
struct Consumer {
  fo::PublisherId publisher{"consumer-publisher"};
  fo::BootGeneration boot{1};
  fo::CoordinatorEpoch epoch{1};
  fo::FederationId federation{"consumer-fed"};
  fo::FederationGeneration federation_generation{1};
  fo::EvidenceGeneration evidence_generation{1};
  fo::Sequence sequence;

  fo::PublicationContext next() {
    fo::PublicationContext context;
    context.publisher = publisher;
    context.boot = boot;
    context.coordinator_epoch = epoch;
    context.federation = federation;
    context.federation_generation = federation_generation;
    sequence = sequence.next();
    context.sequence = sequence;
    context.precision = fo::Precision::Exact;
    context.evidence_class = fo::EvidenceClass::Synthetic;
    context.provenance = fo::Provenance::PublisherReport;
    context.evidence_generation = evidence_generation;
    return context;
  }
};

fo::CapabilityEntry capability(fo::CapabilityKey key, fo::CapabilityValue value) {
  fo::CapabilityEntry entry;
  entry.key.key = key;
  entry.value = std::move(value);
  entry.precision = fo::Precision::Exact;
  entry.evidence_class = fo::EvidenceClass::Synthetic;
  entry.generation = fo::EvidenceGeneration{1};
  return entry;
}

/// A publication that was refused is reported as a classified Error, so an ok() result is
/// an accepted ingest. The disposition still says whether state actually changed.
/// Declare the two capabilities the accelerator class and the cluster both publish.
int put_capabilities(fo::CapabilitySet& set, const char* subject) {
  const fo::Status architecture = set.put(capability(
      fo::CapabilityKey::AcceleratorArchitecture, fo::CapabilityValue::text("sm_90")));
  const fo::Status precision =
      set.put(capability(fo::CapabilityKey::PrecisionFp8E4M3, fo::CapabilityValue::boolean(true)));
  if (!architecture.ok() || !precision.ok()) {
    std::printf("FAILED [%s capabilities]: the declaration was rejected\n", subject);
    return 1;
  }
  return 0;
}

int publish(const fo::Result<fo::IngestResult>& result, const char* what) {
  if (!result.ok()) {
    return fail(result, what);
  }
  std::printf("  %-26s %s\n", what,
              std::string(fo::to_string(result.value().disposition)).c_str());
  return 0;
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: installed consumer\n",
              std::string(fo::version_string()).c_str());
  std::printf("  build banner                 %s\n", std::string(fo::build_banner()).c_str());
  std::printf("  version                      %d.%d.%d\n", fo::version().major, fo::version().minor,
              fo::version().patch);
  std::printf("  persistence_format_version   %u\n", fo::persistence_format_version());
  std::printf("  protocol_version             %u\n", fo::protocol_version());

  section("Bounds");
  const fo::Bounds bounds = fo::default_bounds();
  const fo::Status valid = bounds.validate();
  if (!valid.ok()) {
    return fail(valid, "default bounds");
  }
  std::printf("  max_federations %zu | max_sites %zu | max_clusters %zu | max_workloads %zu\n",
              bounds.max_federations, bounds.max_sites, bounds.max_clusters, bounds.max_workloads);
  std::printf("  max_publishers %zu | max_evidence_per_finding %zu | bounds.validate() ok\n",
              bounds.max_publishers, bounds.max_evidence_per_finding);

  fo::FederationObservatory observatory(fo::ObservatoryConfig{});
  Consumer consumer;
  const fo::PublisherId publisher = consumer.publisher;

  section("publication");
  const fo::Result<fo::IngestResult> registration = observatory.register_publisher(consumer.next());
  if (!registration.ok()) {
    return fail(registration, "register publisher");
  }
  std::printf("  %-26s %s\n", "register_publisher",
              std::string(fo::to_string(registration.value().disposition)).c_str());

  fo::FederationRecord federation;
  federation.id = consumer.federation;
  federation.generation = consumer.federation_generation;
  federation.display_name = "consumer federation";
  federation.coordinator_epoch = consumer.epoch;
  federation.sites = {fo::SiteId::unchecked("site-a")};
  federation.clusters = {fo::ClusterId::unchecked("cluster-a")};
  federation.accelerator_classes = {fo::AcceleratorClassId::unchecked("accel-a")};
  federation.runtimes = {fo::RuntimeId::unchecked("runtime-a")};
  federation.workload_classes = {fo::WorkloadClassId::unchecked("wc-a")};
  if (publish(observatory.register_federation(consumer.next(), federation), "federation")) return 1;

  fo::SiteRecord site;
  site.id = fo::SiteId::unchecked("site-a");
  site.generation = fo::SiteGeneration{1};
  site.federation = consumer.federation;
  site.region = "region-a";
  site.failure_domain = "fd-a";
  site.clusters = {fo::ClusterId::unchecked("cluster-a")};
  if (publish(observatory.register_site(consumer.next(), site), "site")) return 1;

  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = fo::AcceleratorClassId::unchecked("accel-a");
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  accelerator_class.vendor = "vendor-a";
  accelerator_class.family = "family-a";
  accelerator_class.model = "model-a";
  accelerator_class.architecture = "sm_90";
  accelerator_class.compute_capability = "9.0";
  accelerator_class.memory_bytes_per_device = 80 * kGiB;
  accelerator_class.memory_bandwidth_gbps = 3350;
  if (put_capabilities(accelerator_class.capabilities, "accelerator class") != 0) {
    return 1;
  }
  if (publish(observatory.register_accelerator_class(consumer.next(), accelerator_class), "accelerator_class"))
    return 1;

  fo::RuntimeRecord runtime;
  runtime.id = fo::RuntimeId::unchecked("runtime-a");
  runtime.generation = fo::RuntimeGeneration{1};
  runtime.kind = fo::RuntimeKind::Cuda;
  runtime.version = "12.4.1";
  runtime.abi = "cuda-12.4";
  runtime.driver_version = "550.54.15";
  runtime.driver_abi = "550.54";
  if (publish(observatory.register_runtime(consumer.next(), runtime), "runtime")) return 1;

  fo::ClusterRecord cluster;
  cluster.id = fo::ClusterId::unchecked("cluster-a");
  cluster.generation = fo::ClusterGeneration{1};
  cluster.epoch = fo::ClusterEpoch{1};
  cluster.federation = consumer.federation;
  cluster.site = site.id;
  cluster.failure_domain = "fd-a";
  cluster.accelerator_classes = {accelerator_class.id};
  cluster.runtimes = {runtime.id};
  cluster.topology_generation = fo::TopologyGeneration{1};
  cluster.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  cluster.capacity_generation = fo::CapacityGeneration{1};
  cluster.runtime_generation = fo::RuntimeGeneration{1};
  cluster.compatibility_generation = fo::CompatibilityGeneration{1};
  cluster.evidence_generation = fo::EvidenceGeneration{1};
  cluster.readiness = fo::Readiness::Ready;
  if (put_capabilities(cluster.capabilities, "cluster") != 0) {
    return 1;
  }
  if (publish(observatory.register_cluster(consumer.next(), cluster), "cluster")) return 1;

  fo::ArtifactRecord artifact;
  artifact.id = fo::ArtifactId::unchecked("artifact-a");
  artifact.generation = fo::ArtifactGeneration{1};
  artifact.kind = fo::ArtifactKind::DeviceImage;
  artifact.format = "cubin";
  artifact.kernel_format = "cubin";
  artifact.target_architectures = {"sm_90"};
  artifact.minimum_compute_capability = "9.0";
  artifact.compiler_target = "sm_90";
  artifact.runtime_abi = "cuda-12.4";
  artifact.driver_abi = "550.54";
  artifact.required_memory_bytes = 64 * kGiB;
  if (publish(observatory.register_artifact(consumer.next(), artifact), "artifact")) return 1;

  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-a");
  workload_class.display_name = "consumer training";
  if (publish(observatory.register_workload_class(consumer.next(), workload_class), "workload_class"))
    return 1;

  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-a");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.artifact = artifact.id;
  workload.artifact_generation = artifact.generation;
  workload.required_accelerators = 2;
  workload.required_memory_bytes_per_accelerator = 64 * kGiB;
  workload.acceptable_accelerator_classes = {accelerator_class.id};
  workload.required_domain_kind = fo::DomainKind::Cluster;
  fo::CapabilityRequirement requirement;
  requirement.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  requirement.comparator = fo::CapabilityComparator::Present;
  requirement.rationale = "the consumer's kernels are compiled for fp8";
  workload.requirements.push_back(requirement);
  if (publish(observatory.register_workload(consumer.next(), workload), "workload")) return 1;

  section("capacity publication");
  {
    const fo::Result<fo::CapacityLedger> ledger =
        fo::CapacityLedger::from_components(8, 0, 2, 0, 0, 0);
    if (!ledger.ok()) {
      return fail(ledger, "capacity ledger");
    }
    fo::CapacityPool pool;
    pool.pool_id = fo::ResourcePoolId::unchecked("pool-a-accelerators");
    pool.kind = fo::ResourceKind::Accelerator;
    pool.accelerator_class = accelerator_class.id;
    pool.ledger = ledger.value();
    pool.generation = fo::CapacityGeneration{1};
    std::vector<fo::CapacityPool> pools;
    pools.push_back(std::move(pool));
    if (publish(observatory.publish_capacity(consumer.next(), cluster.id, cluster.generation,
                                              fo::CapacityGeneration{1}, std::move(pools)),
                "publish_capacity") != 0) {
      return 1;
    }
    std::printf("  ledger: nominal %llu, allocated %llu, idle %llu\n",
                static_cast<unsigned long long>(ledger.value().nominal),
                static_cast<unsigned long long>(ledger.value().allocated),
                static_cast<unsigned long long>(ledger.value().idle));
  }

  section("placement publication");
  {
    fo::PlacementRecord placement;
    placement.id = fo::PlacementId::unchecked("placement-a");
    placement.generation = fo::PlacementGeneration{1};
    placement.workload = workload.id;
    placement.workload_generation = workload.generation;
    placement.workload_class = workload_class.id;
    placement.federation = consumer.federation;
    placement.federation_generation = consumer.federation_generation;
    placement.selected = cluster.id;
    placement.selected_generation = cluster.generation;
    placement.selected_epoch = cluster.epoch;
    placement.selected_accelerator_class = accelerator_class.id;
    placement.selected_accelerator_count = 2;
    placement.selected_memory_bytes = 2 * 80 * kGiB;
    placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
    placement.capacity_available = true;
    placement.policy_generation = fo::PolicyGeneration{1};
    placement.capacity_generation = fo::CapacityGeneration{1};
    placement.topology_generation = fo::TopologyGeneration{1};
    placement.compatibility_generation = fo::CompatibilityGeneration{1};
    fo::CandidateObservation selected;
    selected.cluster = cluster.id;
    selected.cluster_generation = cluster.generation;
    selected.cluster_epoch = cluster.epoch;
    selected.status = fo::CandidateStatus::Selected;
    selected.basis = fo::ReasonBasis::Observed;
    selected.precision = fo::Precision::Exact;
    placement.candidates.push_back(selected);
    if (publish(observatory.publish_placement(consumer.next(), placement), "publish_placement") != 0) {
      return 1;
    }
  }

  section("placement explanation");
  const fo::Result<fo::PlacementExplanation> explanation =
      observatory.explain_placement(fo::PlacementId::unchecked("placement-a"));
  if (!explanation.ok()) {
    return fail(explanation, "explain placement");
  }
  std::printf("%s\n", explanation.value().render().c_str());

  section("attribution");
  std::printf(
      "  The consumer published every record above and owns every claim in them. The runtime\n"
      "  observed them, reported the placement the consumer's scheduler published, and derived\n"
      "  eligibility from the published evidence. It chose nothing: the candidate set was\n"
      "  exposed as SELECTED_ONLY, so rejection attribution is unavailable, not guessed.\n"
      "  publisher %s remains live: %s\n",
      publisher.value().c_str(),
      observatory.publisher_is_live(publisher, consumer.boot) ? "yes" : "no");
  return 0;
}
