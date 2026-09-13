// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Independent downstream consumer. It includes only installed public headers, links only
// the installed imported target, and exercises the public API end to end: registration,
// capability and capacity publication, a placement observation, an explanation, a
// stranded-capacity analysis and a durable save/load round trip.

#include <cstdio>
#include <string>
#include <vector>

#include <federation_observatory/analysis.hpp>
#include <federation_observatory/observatory.hpp>
#include <federation_observatory/state_store.hpp>
#include <federation_observatory/version.hpp>

namespace {

fo::PublicationContext context(const fo::FederationObservatory& observatory,
                               fo::PublisherId publisher, fo::Sequence sequence) {
  fo::PublicationContext ctx;
  ctx.publisher = publisher;
  ctx.boot = fo::BootGeneration{1};
  ctx.coordinator_epoch = observatory.coordinator_epoch();
  ctx.federation = fo::FederationId::unchecked("consumer-fed");
  ctx.federation_generation = fo::FederationGeneration{1};
  ctx.sequence = sequence;
  ctx.observed_at = fo::now_unix_nanos();
  ctx.precision = fo::Precision::Exact;
  ctx.evidence_class = fo::EvidenceClass::Synthetic;
  ctx.provenance = fo::Provenance::ExternalInventory;
  ctx.evidence_generation = fo::EvidenceGeneration{1};
  return ctx;
}

int fail(const char* what, const fo::Status& status) {
  std::printf("consumer: %s failed: %s\n", what, status.error().to_string().c_str());
  return 1;
}

}  // namespace

int main() {
  std::printf("Federation Observatory consumer built against %s\n",
              std::string(fo::version_string()).c_str());
  std::printf("%s\n", std::string(fo::build_banner()).c_str());

  fo::FederationObservatory observatory;
  const fo::PublisherId publisher = fo::PublisherId::unchecked("consumer-publisher");
  fo::Sequence sequence;

  const auto next = [&]() { sequence = sequence.next(); return context(observatory, publisher, sequence); };
  const auto applied = [](const fo::Result<fo::IngestResult>& result) {
    return result.ok() && result.value().disposition == fo::IngestDisposition::Applied;
  };

  if (!applied(observatory.register_publisher(next()))) {
    return fail("register_publisher", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::FederationRecord federation;
  federation.id = fo::FederationId::unchecked("consumer-fed");
  federation.generation = fo::FederationGeneration{1};
  federation.coordinator_epoch = fo::CoordinatorEpoch{1};
  if (!applied(observatory.register_federation(next(), federation))) {
    return fail("register_federation", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::SiteRecord site;
  site.id = fo::SiteId::unchecked("consumer-site");
  site.generation = fo::SiteGeneration{1};
  site.federation = federation.id;
  if (!applied(observatory.register_site(next(), site))) {
    return fail("register_site", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = fo::AcceleratorClassId::unchecked("consumer-accel");
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  accelerator_class.architecture = "sm_90";
  accelerator_class.memory_bytes_per_device = 80ull * 1024 * 1024 * 1024;
  {
    fo::CapabilityEntry entry;
    entry.key.key = fo::CapabilityKey::ArtifactFormats;
    const fo::Result<fo::CapabilityValue> formats = fo::CapabilityValue::text_set({"cubin"});
    if (!formats.ok()) return 1;
    entry.value = formats.value();
    if (!accelerator_class.capabilities.put(std::move(entry)).ok()) return 1;
  }
  if (!applied(observatory.register_accelerator_class(next(), accelerator_class))) {
    return fail("register_accelerator_class", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::RuntimeRecord runtime;
  runtime.id = fo::RuntimeId::unchecked("consumer-runtime");
  runtime.generation = fo::RuntimeGeneration{1};
  runtime.kind = fo::RuntimeKind::Cuda;
  runtime.abi = "cuda-12.4";
  if (!applied(observatory.register_runtime(next(), runtime))) {
    return fail("register_runtime", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::ClusterRecord cluster;
  cluster.id = fo::ClusterId::unchecked("consumer-cluster");
  cluster.generation = fo::ClusterGeneration{1};
  cluster.epoch = fo::ClusterEpoch{1};
  cluster.federation = federation.id;
  cluster.site = site.id;
  cluster.accelerator_classes = {accelerator_class.id};
  cluster.runtimes = {runtime.id};
  cluster.readiness = fo::Readiness::Ready;
  if (!applied(observatory.register_cluster(next(), cluster))) {
    return fail("register_cluster", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  {
    std::vector<fo::CapacityPool> pools;
    fo::CapacityPool pool;
    pool.pool_id = fo::ResourcePoolId::unchecked("consumer-pool");
    pool.kind = fo::ResourceKind::Accelerator;
    pool.accelerator_class = accelerator_class.id;
    const fo::Result<fo::CapacityLedger> ledger =
        fo::CapacityLedger::from_components(8, 0, 2, 1, 0, 0);
    if (!ledger.ok()) {
      return fail("capacity ledger", ledger.error());
    }
    pool.ledger = ledger.value();
    pools.push_back(std::move(pool));
    if (!applied(observatory.publish_capacity(next(), cluster.id, fo::ClusterGeneration{1},
                                              fo::CapacityGeneration{1}, std::move(pools)))) {
      return fail("publish_capacity", fo::Status::error(fo::ErrorCode::Internal, "refused"));
    }
  }
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("consumer-wc");
  if (!applied(observatory.register_workload_class(next(), workload_class))) {
    return fail("register_workload_class", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::ArtifactRecord artifact;
  artifact.id = fo::ArtifactId::unchecked("consumer-artifact");
  artifact.generation = fo::ArtifactGeneration{1};
  artifact.kind = fo::ArtifactKind::DeviceImage;
  artifact.format = "cubin";
  artifact.target_architectures = {"sm_90"};
  artifact.runtime_abi = "cuda-12.4";
  if (!applied(observatory.register_artifact(next(), artifact))) {
    return fail("register_artifact", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("consumer-wl");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.artifact = artifact.id;
  workload.artifact_generation = fo::ArtifactGeneration{1};
  workload.required_accelerators = 4;
  if (!applied(observatory.register_workload(next(), workload))) {
    return fail("register_workload", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }
  fo::PlacementRecord placement;
  placement.id = fo::PlacementId::unchecked("consumer-placement");
  placement.generation = fo::PlacementGeneration{1};
  placement.workload = workload.id;
  placement.workload_generation = fo::WorkloadGeneration{1};
  placement.federation = federation.id;
  placement.federation_generation = fo::FederationGeneration{1};
  placement.workload_class = workload_class.id;
  placement.selected = cluster.id;
  placement.selected_generation = fo::ClusterGeneration{1};
  placement.selected_epoch = fo::ClusterEpoch{1};
  placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
  placement.capacity_available = true;
  {
    fo::CandidateObservation observation;
    observation.cluster = cluster.id;
    observation.status = fo::CandidateStatus::Selected;
    observation.basis = fo::ReasonBasis::Observed;
    placement.candidates.push_back(observation);
  }
  if (!applied(observatory.publish_placement(next(), placement))) {
    return fail("publish_placement", fo::Status::error(fo::ErrorCode::Internal, "refused"));
  }

  const fo::SnapshotHandle snapshot = observatory.snapshot();
  std::printf("snapshot: %zu cluster(s), %zu placement(s), digest %s\n", snapshot->clusters.size(),
              snapshot->placements.size(), snapshot->digest().c_str());

  const fo::Result<fo::PlacementExplanation> explanation =
      observatory.explain_placement(fo::PlacementId::unchecked("consumer-placement"));
  if (!explanation.ok()) {
    return fail("explain_placement", explanation.error());
  }
  std::printf("explanation digest %s (%zu findings, rejection attribution %s)\n",
              explanation.value().digest().c_str(), explanation.value().findings.size(),
              explanation.value().has_rejection_attribution() ? "available" : "unavailable");

  fo::StrandedCapacityRequest request;
  request.federation = federation.id;
  request.workload_class = workload_class.id;
  const fo::Result<fo::StrandedCapacityReport> stranded = observatory.stranded_capacity(request);
  if (!stranded.ok()) {
    return fail("stranded_capacity", stranded.error());
  }
  std::printf("capacity: nominal %llu idle %llu usable %llu stranded %llu unknown %llu (closes: %s)\n",
              static_cast<unsigned long long>(stranded.value().summary.nominal),
              static_cast<unsigned long long>(stranded.value().summary.idle),
              static_cast<unsigned long long>(stranded.value().summary.usable),
              static_cast<unsigned long long>(stranded.value().summary.stranded),
              static_cast<unsigned long long>(stranded.value().summary.unknown),
              stranded.value().closes() ? "yes" : "no");

  const std::string state_path = std::string("consumer.state");
  const fo::Status saved = observatory.save_state(state_path);
  if (!saved.ok()) {
    return fail("save_state", saved);
  }
  fo::FederationObservatory restored;
  const fo::Status loaded = restored.load_state(state_path);
  if (!loaded.ok()) {
    return fail("load_state", loaded);
  }
  std::printf("persistence: saved and reloaded %zu cluster(s); epoch advanced from %s to %s\n",
              restored.snapshot()->clusters.size(),
              observatory.coordinator_epoch().to_string().c_str(),
              restored.coordinator_epoch().to_string().c_str());
  const fo::Status removed = fo::remove_file_if_present(state_path);
  (void)removed;
  std::printf("consumer: ok\n");
  return 0;
}
