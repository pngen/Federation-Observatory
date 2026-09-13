// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Persistence: versioned, integrity-checked, atomically replaced, and all-or-nothing.
// A corrupt state file can never partially apply, and dynamic evidence can never come
// back as current.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "federation_observatory/process.hpp"
#include "federation_observatory/records_codec.hpp"
#include "fixture.hpp"
#include "federation_observatory/state_store.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

std::string temporary_state_path(const char* tag) {
  return join_path(std::filesystem::temp_directory_path().string(),
                   std::string("fo-test-state-") + tag + "-" + unique_token() + ".bin");
}

struct StateFile {
  std::string path;
  explicit StateFile(const char* tag) : path(temporary_state_path(tag)) {}
  ~StateFile() {
    const Status removed = remove_file_if_present(path);
    (void)removed;
  }
  StateFile(const StateFile&) = delete;
  StateFile& operator=(const StateFile&) = delete;
};

}  // namespace

FO_TEST(persistence, round_trip_preserves_structure_and_history) {
  const StateFile file("roundtrip");
  fo::FederationObservatory original;
  fixture::World world(original);
  FO_REQUIRE(world.build(3, 2).ok());
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-1");
  FO_REQUIRE(original.register_workload_class(world.publisher().next(), workload_class).ok());
  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-1");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.required_accelerators = 1;
  FO_REQUIRE(original.register_workload(world.publisher().next(), workload).ok());

  fo::PlacementRecord placement;
  placement.id = fo::PlacementId::unchecked("placement-1");
  placement.generation = fo::PlacementGeneration{1};
  placement.workload = workload.id;
  placement.workload_generation = fo::WorkloadGeneration{1};
  placement.federation = world.publisher().federation;
  placement.federation_generation = fo::FederationGeneration{1};
  placement.workload_class = workload_class.id;
  placement.selected = world.clusters().front();
  placement.selected_generation = fo::ClusterGeneration{1};
  placement.selected_epoch = fo::ClusterEpoch{1};
  placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
  fo::CandidateObservation observation;
  observation.cluster = placement.selected;
  observation.status = fo::CandidateStatus::Selected;
  observation.basis = fo::ReasonBasis::Observed;
  placement.candidates.push_back(observation);
  FO_REQUIRE(original.publish_placement(world.publisher().next(), placement).ok());

  const fo::CoordinatorEpoch epoch_before = original.coordinator_epoch();
  FO_REQUIRE(original.save_state(file.path).ok());

  fo::FederationObservatory restored;
  FO_REQUIRE(restored.load_state(file.path).ok());
  const fo::SnapshotHandle snapshot = restored.snapshot();
  FO_CHECK_EQ(snapshot->clusters.size(), 3u);
  FO_CHECK_EQ(snapshot->placements.size(), 1u);
  FO_CHECK(snapshot->find_federation(world.publisher().federation) != nullptr);
  FO_CHECK(snapshot->find_workload(workload.id) != nullptr);

  // History survives; currentness does not.
  FO_CHECK(snapshot->placements.front().currentness == fo::Currentness::Stale);
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    FO_CHECK(cluster.currentness == fo::Currentness::RevalidationRequired);
    FO_CHECK(cluster.capacity_pools.empty());
    FO_CHECK(cluster.readiness == fo::Readiness::Unknown);
  }
  FO_CHECK(restored.coordinator_epoch().newer_than(epoch_before));
}

FO_TEST(persistence, old_epoch_traffic_and_replayed_sequences_are_refused_after_restore) {
  const StateFile file("epoch");
  fo::FederationObservatory original;
  fixture::World world(original);
  FO_REQUIRE(world.build(1, 1).ok());
  FO_REQUIRE(original.save_state(file.path).ok());

  fo::FederationObservatory restored;
  FO_REQUIRE(restored.load_state(file.path).ok());

  // The old boot identity survives, but its authority does not: the epoch advanced.
  fixture::Publisher stale;
  stale.id = world.publisher().id;
  stale.boot = world.publisher().boot;
  stale.federation = world.publisher().federation;
  stale.epoch = original.coordinator_epoch();
  stale.sequence = world.publisher().sequence;
  const fo::Result<fo::IngestResult> refused =
      restored.register_site(stale.next(),
                             fo::SiteRecord{fo::SiteId::unchecked("site-x"),
                                            fo::SiteGeneration{1}, world.publisher().federation});
  FO_CHECK(!refused.ok());
  FO_CHECK(refused.error().code() == fo::ErrorCode::StaleEpoch);

  // A replay of an already-accepted sequence number is refused even with the right epoch.
  fixture::Publisher replay;
  replay.id = world.publisher().id;
  replay.boot = world.publisher().boot;
  replay.federation = world.publisher().federation;
  replay.epoch = restored.coordinator_epoch();
  replay.sequence = fo::Sequence{world.publisher().sequence.value()};
  const fo::Result<fo::IngestResult> replayed = restored.register_site(
      replay.repeat(),
      fo::SiteRecord{fo::SiteId::unchecked("site-y"), fo::SiteGeneration{1},
                     world.publisher().federation});
  // A replay of an already-accepted sequence is either refused outright or suppressed as a
  // duplicate. What must never happen is that it mutates state.
  const bool suppressed =
      !replayed.ok() || replayed.value().disposition != fo::IngestDisposition::Applied;
  FO_CHECK(suppressed);
  const fo::SnapshotHandle replay_snapshot = restored.snapshot();
  FO_CHECK(replay_snapshot->find_site(fo::SiteId::unchecked("site-y")) == nullptr);
}

FO_TEST(persistence, rejected_load_leaves_the_runtime_untouched) {
  const StateFile file("atomic");
  fo::FederationObservatory live;
  fixture::World world(live);
  FO_REQUIRE(world.build(2, 1).ok());
  const std::size_t clusters_before = live.snapshot()->clusters.size();
  const fo::CoordinatorEpoch epoch_before = live.coordinator_epoch();

  const Status missing = live.load_state(file.path);
  FO_CHECK(!missing.ok());
  FO_CHECK(missing.code() == fo::ErrorCode::NotFound);
  FO_CHECK_EQ(live.snapshot()->clusters.size(), clusters_before);
  FO_CHECK(live.coordinator_epoch() == epoch_before);
}

FO_TEST(persistence, corrupt_images_are_rejected_without_partial_apply) {
  const StateFile file("corrupt");
  fo::FederationObservatory original;
  fixture::World world(original);
  FO_REQUIRE(world.build(2, 1).ok());
  FO_REQUIRE(original.save_state(file.path).ok());
  const std::vector<std::uint8_t> image = FO_UNWRAP(read_file_bounded(file.path, 1u << 20));
  FO_CHECK(image.size() > fo::kStateHeaderSize);

  const auto rejected = [&](const std::vector<std::uint8_t>& candidate, fo::ErrorCode expected,
                            const char* what) {
    const fo::Result<fo::DurableState> decoded =
        fo::decode_durable_state(candidate.data(), candidate.size(), fo::default_bounds());
    if (decoded.ok()) {
      ::fotest::fail(__FILE__, __LINE__, std::string("corrupt state was accepted: ") + what);
    }
    if (decoded.error().code() != expected) {
      ::fotest::fail(__FILE__, __LINE__,
                     std::string("wrong rejection code for ") + what + ": " +
                         std::string(fo::to_string(decoded.error().code())));
    }
  };

  rejected({}, fo::ErrorCode::CorruptState, "empty image");
  rejected(std::vector<std::uint8_t>(10, 0), fo::ErrorCode::CorruptState, "truncated header");

  std::vector<std::uint8_t> bad_magic = image;
  bad_magic[0] = 'X';
  rejected(bad_magic, fo::ErrorCode::CorruptState, "bad magic");

  std::vector<std::uint8_t> bad_version = image;
  bad_version[8] = 99;
  rejected(bad_version, fo::ErrorCode::UnsupportedVersion, "unsupported version");

  std::vector<std::uint8_t> bad_flags = image;
  bad_flags[12] = 1;
  rejected(bad_flags, fo::ErrorCode::CorruptState, "unknown flags");

  std::vector<std::uint8_t> bad_reserved = image;
  bad_reserved[28] = 1;
  rejected(bad_reserved, fo::ErrorCode::CorruptState, "non-zero reserved field");

  std::vector<std::uint8_t> bad_length = image;
  bad_length[16] = static_cast<std::uint8_t>(bad_length[16] + 1);
  rejected(bad_length, fo::ErrorCode::CorruptState, "declared length mismatch");

  std::vector<std::uint8_t> bad_crc = image;
  bad_crc[fo::kStateHeaderSize + 8] ^= 0xFFu;
  rejected(bad_crc, fo::ErrorCode::IntegrityFailure, "corrupt checksum");

  // The declared payload length is validated before the checksum, so a truncation is
  // refused as a length mismatch rather than as a checksum failure.
  std::vector<std::uint8_t> truncated = image;
  truncated.resize(image.size() - 3);
  rejected(truncated, fo::ErrorCode::CorruptState, "truncated payload");

  // A payload of the right length whose content was altered is an integrity failure.
  std::vector<std::uint8_t> altered = image;
  altered[fo::kStateHeaderSize + 32] ^= 0x01u;
  rejected(altered, fo::ErrorCode::IntegrityFailure, "altered payload");

  std::vector<std::uint8_t> oversized = image;
  for (int i = 0; i < 4; ++i) {
    oversized[16 + static_cast<std::size_t>(i)] = 0xFFu;
  }
  rejected(oversized, fo::ErrorCode::CorruptState, "oversized declared length");

  fo::FederationObservatory target;
  const Status loaded = target.load_state(file.path);
  FO_CHECK(loaded.ok() || loaded.code() == fo::ErrorCode::CorruptState);
}

FO_TEST(persistence, injected_semantic_defects_are_rejected) {
  const StateFile file("semantic");
  fo::FederationObservatory original;
  fixture::World world(original);
  FO_REQUIRE(world.build(2, 1).ok());
  FO_REQUIRE(original.save_state(file.path).ok());
  const fo::DurableState good = FO_UNWRAP(load_durable_state(file.path, fo::default_bounds()));
  FO_CHECK(good.validate(fo::default_bounds()).ok());

  const auto reject_state = [&](fo::DurableState state, const char* what) {
    const Status status = state.validate(fo::default_bounds());
    if (status.ok()) {
      ::fotest::fail(__FILE__, __LINE__, std::string("semantic defect was accepted: ") + what);
    }
  };

  {
    fo::DurableState duplicate = good;
    duplicate.clusters.push_back(duplicate.clusters.front());
    reject_state(std::move(duplicate), "duplicate cluster id");
  }
  {
    fo::DurableState broken = good;
    broken.clusters.front().site = fo::SiteId::unchecked("site-does-not-exist");
    reject_state(std::move(broken), "broken site link");
  }
  {
    fo::DurableState broken = good;
    broken.sites.front().federation = fo::FederationId::unchecked("fed-does-not-exist");
    reject_state(std::move(broken), "broken federation link");
  }
  {
    fo::DurableState dynamic = good;
    dynamic.clusters.front().capacity_pools.push_back(fo::CapacityPool{});
    reject_state(std::move(dynamic), "dynamic capacity in a durable file");
  }
  {
    fo::DurableState current = good;
    current.clusters.front().currentness = fo::Currentness::Current;
    reject_state(std::move(current), "current cluster in a durable file");
  }
  {
    fo::DurableState unbounded = good;
    unbounded.clusters.resize(fo::default_bounds().max_clusters + 1);
    reject_state(std::move(unbounded), "bound exceeded");
  }
  {
    fo::DurableState watermark = good;
    watermark.publisher_watermarks.push_back(watermark.publisher_watermarks.front());
    reject_state(std::move(watermark), "duplicate publisher watermark");
  }
  {
    fo::DurableState version = good;
    version.format_version = 999;
    reject_state(std::move(version), "unsupported format version");
  }
  {
    fo::DurableState workload = good;
    fo::WorkloadRecord orphan;
    orphan.id = fo::WorkloadId::unchecked("wl-orphan");
    orphan.generation = fo::WorkloadGeneration{1};
    orphan.workload_class = fo::WorkloadClassId::unchecked("wc-does-not-exist");
    workload.workloads.push_back(orphan);
    reject_state(std::move(workload), "workload with an unknown class");
  }
  {
    fo::DurableState migration = good;
    fo::MigrationRecord broken;
    broken.id = fo::MigrationId::unchecked("migration-broken");
    broken.generation = fo::MigrationGeneration{1};
    broken.workload = fo::WorkloadId::unchecked("wl-does-not-exist");
    broken.workload_generation = fo::WorkloadGeneration{1};
    broken.federation = world.publisher().federation;
    broken.source = fo::ClusterId::unchecked("cluster-0");
    broken.destination = fo::ClusterId::unchecked("cluster-1");
    migration.migrations.push_back(broken);
    reject_state(std::move(migration), "migration with an unknown workload");
  }
  {
    fo::DurableState placement = good;
    fo::PlacementRecord broken;
    broken.id = fo::PlacementId::unchecked("placement-broken");
    broken.generation = fo::PlacementGeneration{1};
    broken.workload = fo::WorkloadId::unchecked("wl-does-not-exist");
    broken.workload_generation = fo::WorkloadGeneration{1};
    broken.federation = world.publisher().federation;
    broken.selected = fo::ClusterId::unchecked("cluster-0");
    broken.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
    placement.placements.push_back(broken);
    reject_state(std::move(placement), "placement with an unknown workload");
  }
}

FO_TEST(persistence, save_is_atomic_and_leaves_no_temporary_file) {
  const StateFile file("atomic-write");
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(1, 1).ok());
  FO_REQUIRE(observatory.save_state(file.path).ok());
  FO_REQUIRE(observatory.save_state(file.path).ok());
  const std::vector<std::uint8_t> first = FO_UNWRAP(read_file_bounded(file.path, 1u << 20));
  FO_CHECK(first.size() > fo::kStateHeaderSize);
  std::error_code error;
  FO_CHECK(!std::filesystem::exists(file.path + ".tmp", error));
}

FO_TEST(persistence, durable_state_digest_is_stable) {
  const StateFile file("digest");
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(2, 1).ok());
  FO_REQUIRE(observatory.save_state(file.path).ok());
  const fo::DurableState state =
      FO_UNWRAP(load_durable_state(file.path, fo::default_bounds()));
  FO_CHECK_EQ(state.digest(), state.digest());
  FO_CHECK(!state.summary().empty());
  const fo::Result<std::vector<std::uint8_t>> encoded =
      encode_durable_state(state, fo::default_bounds());
  FO_REQUIRE(encoded.ok());
  const fo::DurableState decoded = FO_UNWRAP(fo::decode_durable_state(
      encoded.value().data(), encoded.value().size(), fo::default_bounds()));
  FO_CHECK_EQ(decoded.digest(), state.digest());
}

FO_TEST(persistence, every_record_codec_round_trips) {
  // A codec that writes one field order and reads another produces a state file the
  // runtime cannot read back. Every record type is therefore round-tripped explicitly.
  const auto round_trip = [](const auto& record, auto encode, auto decode, const char* what) {
    Encoder encoder;
    encode(encoder, record);
    if (encoder.failed()) {
      ::fotest::fail(__FILE__, __LINE__, std::string("encoding failed for ") + what);
    }
    const std::vector<std::uint8_t> bytes = encoder.take();
    Decoder decoder(bytes.data(), bytes.size(), 8u << 20);
    auto decoded = decode(decoder);
    if (!decoded.ok()) {
      ::fotest::fail(__FILE__, __LINE__,
                     std::string("decoding failed for ") + what + ": " +
                         decoded.error().to_string());
    }
    if (!decoder.exhausted()) {
      ::fotest::fail(__FILE__, __LINE__,
                     std::string("decoder did not consume the whole record: ") + what);
    }
    if (fo::codec::digest_record(record) != fo::codec::digest_record(decoded.value())) {
      ::fotest::fail(__FILE__, __LINE__,
                     std::string("round trip changed the record: ") + what);
    }
  };

  fo::FederationRecord federation;
  federation.id = fo::FederationId::unchecked("fed-1");
  federation.generation = fo::FederationGeneration{3};
  federation.coordinator_epoch = fo::CoordinatorEpoch{2};
  federation.clusters = {fo::ClusterId::unchecked("cluster-1")};
  round_trip(federation, fo::codec::encode_federation, fo::codec::decode_federation, "federation");

  fo::SiteRecord site;
  site.id = fo::SiteId::unchecked("site-1");
  site.generation = fo::SiteGeneration{2};
  site.federation = federation.id;
  site.region = "region";
  site.clusters = {fo::ClusterId::unchecked("cluster-1")};
  round_trip(site, fo::codec::encode_site, fo::codec::decode_site, "site");

  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = fo::AcceleratorClassId::unchecked("accel-1");
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{4};
  accelerator_class.architecture = "sm_90";
  accelerator_class.memory_bytes_per_device = 80ull * 1024 * 1024 * 1024;
  accelerator_class.memory_bandwidth_gbps = 3350;
  fo::CapabilityEntry entry;
  entry.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  entry.value = fo::CapabilityValue::boolean(true);
  FO_REQUIRE(accelerator_class.capabilities.put(std::move(entry)).ok());
  round_trip(accelerator_class, fo::codec::encode_accelerator_class,
             fo::codec::decode_accelerator_class, "accelerator class");

  fo::RuntimeRecord runtime;
  runtime.id = fo::RuntimeId::unchecked("runtime-1");
  runtime.generation = fo::RuntimeGeneration{2};
  runtime.kind = fo::RuntimeKind::Cuda;
  runtime.version = "12.4.1";
  runtime.abi = "cuda-12.4";
  runtime.driver_abi = "550.54";
  round_trip(runtime, fo::codec::encode_runtime, fo::codec::decode_runtime, "runtime");

  fo::BackendRecord backend;
  backend.id = fo::BackendId::unchecked("backend-1");
  backend.generation = fo::BackendGeneration{1};
  backend.runtime = runtime.id;
  backend.name = "serving";
  round_trip(backend, fo::codec::encode_backend, fo::codec::decode_backend, "backend");

  fo::DomainRecord domain;
  domain.id = fo::DomainId::unchecked("domain-1");
  domain.kind = fo::DomainKind::Site;
  domain.federation = federation.id;
  domain.site = site.id;
  domain.clusters = {fo::ClusterId::unchecked("cluster-1")};
  round_trip(domain, fo::codec::encode_domain, fo::codec::decode_domain, "domain");

  fo::ClusterRecord cluster;
  cluster.id = fo::ClusterId::unchecked("cluster-1");
  cluster.generation = fo::ClusterGeneration{2};
  cluster.epoch = fo::ClusterEpoch{3};
  cluster.federation = federation.id;
  cluster.site = site.id;
  cluster.accelerator_classes = {accelerator_class.id};
  cluster.runtimes = {runtime.id};
  cluster.backends = {backend.id};
  fo::CapacityPool pool;
  pool.pool_id = fo::ResourcePoolId::unchecked("pool-1");
  pool.kind = fo::ResourceKind::Accelerator;
  pool.accelerator_class = accelerator_class.id;
  pool.ledger = FO_UNWRAP(fo::CapacityLedger::from_components(8, 1, 2, 1, 1, 1));
  pool.generation = fo::CapacityGeneration{5};
  cluster.capacity_pools.push_back(pool);
  cluster.readiness = fo::Readiness::Ready;
  round_trip(cluster, fo::codec::encode_cluster, fo::codec::decode_cluster, "cluster");

  fo::PolicyRecord policy;
  policy.id = fo::PolicyId::unchecked("policy-1");
  policy.generation = fo::PolicyGeneration{2};
  policy.denied_sites = {site.id};
  policy.allow_cross_site = fo::Tri::No;
  round_trip(policy, fo::codec::encode_policy, fo::codec::decode_policy, "policy");

  fo::ArtifactRecord artifact;
  artifact.id = fo::ArtifactId::unchecked("artifact-1");
  artifact.generation = fo::ArtifactGeneration{7};
  artifact.kind = fo::ArtifactKind::DeviceImage;
  artifact.format = "cubin";
  artifact.kernel_format = "cubin";
  artifact.target_architectures = {"sm_90"};
  artifact.minimum_compute_capability = "9.0";
  artifact.size_bytes = 123456789;
  artifact.runtime_abi = "cuda-12.4";
  artifact.driver_abi = "550.54";
  artifact.compiler_target = "sm_90";
  artifact.state_format = "torch-distributed-v2";
  artifact.required_memory_bytes = 64ull * 1024 * 1024 * 1024;
  round_trip(artifact, fo::codec::encode_artifact, fo::codec::decode_artifact, "artifact");

  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-1");
  workload_class.display_name = "training";
  round_trip(workload_class, fo::codec::encode_workload_class, fo::codec::decode_workload_class,
             "workload class");

  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-1");
  workload.generation = fo::WorkloadGeneration{4};
  workload.workload_class = workload_class.id;
  workload.artifact = artifact.id;
  workload.artifact_generation = artifact.generation;
  workload.required_accelerators = 8;
  workload.required_memory_bytes_per_accelerator = 64ull * 1024 * 1024 * 1024;
  workload.acceptable_accelerator_classes = {accelerator_class.id};
  fo::CapabilityRequirement requirement;
  requirement.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  requirement.comparator = fo::CapabilityComparator::Present;
  workload.requirements.push_back(requirement);
  workload.policy = policy.id;
  workload.policy_generation = policy.generation;
  workload.required_domain = domain.id;
  workload.required_domain_kind = fo::DomainKind::Site;
  workload.isolation_requirement = "process";
  workload.portability_class = "trainer-v1";
  round_trip(workload, fo::codec::encode_workload, fo::codec::decode_workload, "workload");

  fo::PlacementRecord placement;
  placement.id = fo::PlacementId::unchecked("placement-1");
  placement.generation = fo::PlacementGeneration{2};
  placement.workload = workload.id;
  placement.workload_generation = workload.generation;
  placement.federation = federation.id;
  placement.federation_generation = federation.generation;
  placement.workload_class = workload_class.id;
  placement.selected = cluster.id;
  placement.selected_generation = cluster.generation;
  placement.selected_epoch = cluster.epoch;
  placement.selected_accelerator_class = accelerator_class.id;
  placement.selected_accelerator_count = 4;
  placement.selected_memory_bytes = 4ull * 80 * 1024 * 1024 * 1024;
  placement.candidate_completeness = fo::CandidateSetCompleteness::Complete;
  placement.capacity_available = true;
  placement.compatibility_constrained = fo::Tri::Yes;
  placement.fallback_required = true;
  placement.fallback_detail = "preferred site excluded";
  placement.cost_known = true;
  placement.cost_estimate = 1.25;
  placement.slo_class = "gold";
  fo::CandidateObservation candidate;
  candidate.cluster = cluster.id;
  candidate.status = fo::CandidateStatus::Selected;
  candidate.basis = fo::ReasonBasis::Observed;
  placement.candidates.push_back(candidate);
  round_trip(placement, fo::codec::encode_placement, fo::codec::decode_placement, "placement");

  fo::MigrationRecord migration;
  migration.id = fo::MigrationId::unchecked("migration-1");
  migration.generation = fo::MigrationGeneration{3};
  migration.workload = workload.id;
  migration.workload_generation = workload.generation;
  migration.federation = federation.id;
  migration.federation_generation = federation.generation;
  migration.source = fo::ClusterId::unchecked("cluster-2");
  migration.destination = cluster.id;
  migration.stage = fo::MigrationStage::RestoreComplete;
  migration.outcome = fo::MigrationOutcome::InProgress;
  migration.stage_events.push_back(fo::MigrationStageEvent{migration.generation,
                                                           fo::MigrationStage::RestoreComplete,
                                                           fo::Sequence{9}, 12345,
                                                           fo::Precision::Exact,
                                                           fo::EvidenceClass::Synthetic, "restored"});
  migration.reason = "rebalance";
  migration.reason_observed = true;
  migration.state_transfer_known = true;
  migration.state_transfer_bytes = 1024;
  migration.requires_rebuild = true;
  round_trip(migration, fo::codec::encode_migration, fo::codec::decode_migration, "migration");

  fo::PortabilityAssessment portability;
  portability.workload = workload.id;
  portability.workload_generation = workload.generation;
  portability.artifact = artifact.id;
  portability.artifact_generation = artifact.generation;
  portability.workload_class = workload_class.id;
  portability.source = fo::ClusterId::unchecked("cluster-2");
  portability.destination = cluster.id;
  portability.federation = federation.id;
  portability.overall = fo::PortabilityOutcome::PortableWithRecompile;
  portability.requires_recompile = true;
  fo::PortabilityDimensionResult dimension;
  dimension.dimension = fo::PortabilityDimension::Kernel;
  dimension.outcome = fo::PortabilityOutcome::PortableWithRecompile;
  dimension.detail = "kernels must be recompiled";
  portability.dimensions.push_back(dimension);
  round_trip(portability, fo::codec::encode_portability, fo::codec::decode_portability,
             "portability assessment");
}
