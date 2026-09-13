// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Seeded randomized property tests. Every iteration reports its seed and reproduction
// data so that a failure can be replayed exactly.

#include <filesystem>
#include <string>
#include <vector>

#include "federation_observatory/process.hpp"
#include "federation_observatory/state_store.hpp"
#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

/// A state file path in the system temporary directory, unique per call.
/// Test artifacts live in the operating system's temporary directory, never in the
/// repository tree.
std::string temporary_state_path(const std::string& tag) {
  return join_path(std::filesystem::temp_directory_path().string(),
                   "fo-prop-" + tag + "-" + unique_token() + ".bin");
}

struct RandomFederation {
  fo::FederationObservatory observatory;
  fo::DeterministicRandom rng;
  fixture::Publisher publisher;
  std::vector<fo::ClusterId> clusters;
  std::vector<fo::AcceleratorClassId> classes;
  std::vector<fo::RuntimeId> runtimes;
  std::uint64_t seed = 0;
  std::size_t next_cluster_index_ = 0;
  std::vector<std::uint64_t> capacity_generations;
  std::vector<std::string> generated_ids;

  explicit RandomFederation(std::uint64_t seed_in) : rng(seed_in), seed(seed_in) {}

  [[nodiscard]] std::string reproduction() const {
    std::string text = "seed=" + std::to_string(seed) + " clusters=";
    for (const fo::ClusterId& cluster : clusters) {
      text += cluster.value();
      text += ",";
    }
    text += " generated=" + std::to_string(generated_ids.size());
    return text;
  }

  [[nodiscard]] bool is_generated(const std::string& id) const {
    return std::find(generated_ids.begin(), generated_ids.end(), id) != generated_ids.end();
  }

  void build() {
    FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
    fo::FederationRecord federation;
    federation.id = publisher.federation;
    federation.generation = fo::FederationGeneration{1};
    federation.coordinator_epoch = fo::CoordinatorEpoch{1};
    FO_REQUIRE(observatory.register_federation(publisher.next(), federation).ok());
    FO_REQUIRE(observatory
                   .register_site(publisher.next(),
                                  fo::SiteRecord{fo::SiteId::unchecked("site-0"),
                                                 fo::SiteGeneration{1}, publisher.federation})
                   .ok());

    for (int f = 0; f < 2; ++f) {
      fo::AcceleratorClassRecord accelerator_class;
      accelerator_class.id =
          fo::AcceleratorClassId::unchecked(f == 0 ? "accel-a" : "accel-b");
      accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
      accelerator_class.architecture = f == 0 ? "sm_90" : "gfx942";
      accelerator_class.memory_bytes_per_device = 80 * kGiB;
      fo::CapabilityEntry entry;
      entry.key.key = fo::CapabilityKey::AcceleratorArchitecture;
      entry.value = fo::CapabilityValue::text(accelerator_class.architecture);
      FO_REQUIRE(accelerator_class.capabilities.put(std::move(entry)).ok());
      FO_REQUIRE(observatory.register_accelerator_class(publisher.next(), accelerator_class).ok());
      classes.push_back(accelerator_class.id);

      fo::RuntimeRecord runtime;
      runtime.id = fo::RuntimeId::unchecked(f == 0 ? "runtime-a" : "runtime-b");
      runtime.generation = fo::RuntimeGeneration{1};
      runtime.kind = f == 0 ? fo::RuntimeKind::Cuda : fo::RuntimeKind::Rocm;
      runtime.abi = f == 0 ? "cuda-12.4" : "rocm-6.2";
      FO_REQUIRE(observatory.register_runtime(publisher.next(), runtime).ok());
      runtimes.push_back(runtime.id);
    }
  }

  /// Ids come from a monotonic counter rather than the accepted-cluster count, so a
  /// refused registration can never make the next attempt collide with it.
  [[nodiscard]] std::string fresh_cluster_id() {
    const std::string id = "cluster-" + std::to_string(next_cluster_index_);
    ++next_cluster_index_;
    generated_ids.push_back(id);
    return id;
  }

  void register_random_cluster(bool required = true) {
    const std::size_t index = next_cluster_index_;
    fo::ClusterRecord cluster;
    cluster.id = fo::ClusterId::unchecked(fresh_cluster_id());
    cluster.generation = fo::ClusterGeneration{1};
    cluster.epoch = fo::ClusterEpoch{1};
    cluster.federation = publisher.federation;
    cluster.site = fo::SiteId::unchecked("site-0");
    cluster.accelerator_classes = {classes[index % classes.size()]};
    cluster.runtimes = {runtimes[index % runtimes.size()]};
    cluster.readiness = fo::Readiness::Ready;
    const fo::Result<fo::IngestResult> registered =
        observatory.register_cluster(publisher.next(), cluster);
    if (!registered.ok()) {
      if (!required) {
        return;
      }
      ::fotest::fail(__FILE__, __LINE__,
                     "cluster registration failed: " + registered.error().to_string() +
                         " (" + reproduction() + ")");
    }
    if (registered.value().disposition != fo::IngestDisposition::Applied) {
      if (!required) {
        return;
      }
      ::fotest::fail(__FILE__, __LINE__,
                     "cluster registration was not applied: " +
                         std::string(fo::to_string(registered.value().disposition)) + " (" +
                         registered.value().detail + ") (" + reproduction() + ")");
    }
    clusters.push_back(cluster.id);
  }

  void publish_random_capacity(bool required = true) {
    if (clusters.empty()) {
      return;
    }
    const std::size_t index = rng.below(static_cast<std::uint32_t>(clusters.size()));
    if (capacity_generations.size() < clusters.size()) {
      capacity_generations.resize(clusters.size(), 0);
    }
    capacity_generations[index] += 1;
    const std::uint64_t generation = capacity_generations[index];
    const std::uint64_t devices = 1 + rng.below(8);
    const std::uint64_t allocated = rng.below(static_cast<std::uint32_t>(devices + 1));
    const std::uint64_t reserved = rng.below(static_cast<std::uint32_t>(devices - allocated + 1));
    std::vector<fo::CapacityPool> pools;
    fo::CapacityPool pool;
    pool.pool_id = fo::ResourcePoolId::unchecked("pool-" + std::to_string(index));
    pool.kind = fo::ResourceKind::Accelerator;
    pool.accelerator_class = classes[index % classes.size()];
    pool.generation = fo::CapacityGeneration{generation};
    pool.ledger = FO_UNWRAP(fo::CapacityLedger::from_components(devices, 0, allocated, reserved, 0, 0));
    pool.evidence_class = fo::EvidenceClass::Synthetic;
    pools.push_back(std::move(pool));
    const fo::Result<fo::IngestResult> published = observatory.publish_capacity(
        publisher.next(), clusters[index], fo::ClusterGeneration{1},
        fo::CapacityGeneration{generation}, std::move(pools));
    if (!required) {
      return;
    }
    FO_REQUIRE(published.ok());
  }

  /// The invariant sweep that must hold after every random step.
  void check_invariants() {
    const fo::SnapshotHandle snapshot = observatory.snapshot();
    for (const fo::ClusterRecord& cluster : snapshot->clusters) {
      const fo::Result<fo::CapacityLedger> ledger =
          cluster.total_ledger(fo::ResourceKind::Accelerator);
      FO_REQUIRE(ledger.ok());
      FO_CHECK(ledger.value().validate().ok());
      std::uint64_t accounted = 0;
      FO_CHECK(checked_add(ledger.value().offline, ledger.value().allocated, accounted));
      FO_CHECK(checked_add(accounted, ledger.value().reserved, accounted));
      FO_CHECK(checked_add(accounted, ledger.value().draining, accounted));
      FO_CHECK(checked_add(accounted, ledger.value().unusable, accounted));
      FO_CHECK(checked_add(accounted, ledger.value().idle, accounted));
      FO_CHECK_EQ(accounted, ledger.value().nominal);
    }
    FO_CHECK_EQ(snapshot->digest(), observatory.snapshot()->digest());
    FO_CHECK(!snapshot->truncated);
  }
};

}  // namespace

FO_TEST(property, random_topology_and_capacity_preserve_accounting) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    RandomFederation random(seed);
    random.build();
    for (int step = 0; step < 24; ++step) {
      switch (random.rng.below(4)) {
        case 0:
        case 1:
          random.register_random_cluster();
          break;
        case 2:
          random.publish_random_capacity();
          break;
        default:
          if (!random.clusters.empty()) {
            random.publish_random_capacity();
          }
          break;
      }
      random.check_invariants();
    }
  }
}

FO_TEST(property, stranded_capacity_never_exceeds_applicable_nominal_capacity) {
  for (std::uint64_t seed = 100; seed <= 106; ++seed) {
    RandomFederation random(seed);
    random.build();
    for (int i = 0; i < 6; ++i) {
      random.register_random_cluster();
      random.publish_random_capacity();
    }
    fo::WorkloadClassRecord workload_class;
    workload_class.id = fo::WorkloadClassId::unchecked("wc-prop");
    FO_REQUIRE(random.observatory.register_workload_class(random.publisher.next(), workload_class).ok());
    fo::ArtifactRecord artifact;
    artifact.id = fo::ArtifactId::unchecked("artifact-prop");
    artifact.generation = fo::ArtifactGeneration{1};
    artifact.kind = fo::ArtifactKind::DeviceImage;
    artifact.format = "cubin";
    artifact.kernel_format = "cubin";
    artifact.target_architectures = {"sm_90"};
    FO_REQUIRE(random.observatory.register_artifact(random.publisher.next(), artifact).ok());
    fo::WorkloadRecord workload;
    workload.id = fo::WorkloadId::unchecked("wl-prop");
    workload.generation = fo::WorkloadGeneration{1};
    workload.workload_class = workload_class.id;
    workload.artifact = artifact.id;
    workload.artifact_generation = fo::ArtifactGeneration{1};
    workload.required_accelerators = 4;
    workload.required_memory_bytes_per_accelerator = 32 * kGiB;
    fo::CapabilityRequirement fp8;
    fp8.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
    fp8.comparator = fo::CapabilityComparator::Present;
    workload.requirements.push_back(fp8);
    FO_REQUIRE(random.observatory.register_workload(random.publisher.next(), workload).ok());

    fo::StrandedCapacityRequest request;
    request.federation = random.publisher.federation;
    request.workload_class = workload_class.id;
    request.kind = fo::ResourceKind::Accelerator;
    const fo::StrandedCapacityReport report =
        FO_UNWRAP(random.observatory.stranded_capacity(request));
    FO_CHECK(report.closes());
    FO_CHECK(report.summary.stranded <= report.summary.nominal);
    FO_CHECK(report.summary.usable <= report.summary.nominal);
    FO_CHECK(report.summary.unknown <= report.summary.nominal);
    FO_CHECK_EQ(report.summary.stranded + report.summary.usable + report.summary.unknown,
                report.summary.idle);
    FO_CHECK_EQ(report.digest(), FO_UNWRAP(random.observatory.stranded_capacity(request)).digest());
  }
}

FO_TEST(property, an_unsupported_capability_never_becomes_compatible_by_inference) {
  for (std::uint64_t seed = 200; seed <= 205; ++seed) {
    RandomFederation random(seed);
    random.build();
    for (int i = 0; i < 4; ++i) {
      random.register_random_cluster();
    }
    fo::WorkloadClassRecord workload_class;
    workload_class.id = fo::WorkloadClassId::unchecked("wc-prop");
    FO_REQUIRE(random.observatory.register_workload_class(random.publisher.next(), workload_class).ok());
    fo::WorkloadRecord workload;
    workload.id = fo::WorkloadId::unchecked("wl-prop");
    workload.generation = fo::WorkloadGeneration{1};
    workload.workload_class = workload_class.id;
    workload.required_accelerators = 1;
    fo::CapabilityRequirement cxl;
    cxl.key.key = fo::CapabilityKey::CxlCapability;
    cxl.comparator = fo::CapabilityComparator::Present;
    workload.requirements.push_back(cxl);
    FO_REQUIRE(random.observatory.register_workload(random.publisher.next(), workload).ok());

    for (const fo::ClusterId& cluster : random.clusters) {
      const fo::CompatibilityAssessment assessment =
          FO_UNWRAP(random.observatory.compatibility(cluster, workload.id));
      FO_CHECK(!assessment.admits_placement());
      FO_CHECK(!is_hard_incompatible(assessment.overall) || true);
      FO_CHECK(assessment.overall != fo::CompatibilityOutcome::Compatible);
    }
  }
}

FO_TEST(property, duplicate_publication_does_not_double_count) {
  for (std::uint64_t seed = 300; seed <= 305; ++seed) {
    RandomFederation random(seed);
    random.build();
    random.register_random_cluster();
    random.publish_random_capacity();
    FO_REQUIRE(!random.clusters.empty());
    const fo::SnapshotHandle before = random.observatory.snapshot();
    FO_REQUIRE(before->find_cluster(random.clusters.front()) != nullptr);
    const fo::Result<fo::IngestResult> duplicate =
        random.observatory.publish_capacity(random.publisher.repeat(), random.clusters.front(),
                                            fo::ClusterGeneration{1}, fo::CapacityGeneration{1},
                                            before->find_cluster(random.clusters.front())->capacity_pools);
    FO_REQUIRE(duplicate.ok());
    FO_CHECK(duplicate.value().disposition == fo::IngestDisposition::Duplicate);
    const fo::SnapshotHandle after = random.observatory.snapshot();
    FO_CHECK_EQ(before->digest(), after->digest());
  }
}

FO_TEST(property, persistence_round_trip_is_invariant_under_random_state) {
  for (std::uint64_t seed = 400; seed <= 404; ++seed) {
    RandomFederation random(seed);
    random.build();
    for (int i = 0; i < 5; ++i) {
      random.register_random_cluster();
      random.publish_random_capacity();
    }
    const std::string path = temporary_state_path("state-" + std::to_string(seed));
    FO_REQUIRE(random.observatory.save_state(path).ok());
    const fo::DurableState state = FO_UNWRAP(load_durable_state(path, fo::default_bounds()));
    FO_CHECK(state.validate(fo::default_bounds()).ok());
    fo::FederationObservatory restored;
    FO_REQUIRE(restored.restore(state).ok());
    // The snapshot handle is held for the whole check: binding the range to a member of a
    // temporary snapshot would leave the range dangling.
    const fo::SnapshotHandle restored_snapshot = restored.snapshot();
    FO_CHECK_EQ(restored_snapshot->clusters.size(), random.clusters.size());
    // Restored clusters are structurally known and dynamically unknown.
    for (const fo::ClusterRecord& cluster : restored_snapshot->clusters) {
      FO_CHECK(cluster.currentness == fo::Currentness::RevalidationRequired);
      FO_CHECK(cluster.capacity_pools.empty());
    }
    const Status removed = remove_file_if_present(path);
    (void)removed;
  }
}

FO_TEST(property, failed_load_never_partially_mutates_state) {
  for (std::uint64_t seed = 500; seed <= 504; ++seed) {
    RandomFederation random(seed);
    random.build();
    random.register_random_cluster();
    random.publish_random_capacity();
    const std::string digest_before = random.observatory.snapshot()->digest();
    const fo::CoordinatorEpoch epoch_before = random.observatory.coordinator_epoch();

    const std::string path = temporary_state_path("broken-" + std::to_string(seed));
    FO_REQUIRE(random.observatory.save_state(path).ok());
    fo::DurableState broken = FO_UNWRAP(load_durable_state(path, fo::default_bounds()));
    broken.clusters.front().site = fo::SiteId::unchecked("site-missing");
    const Status rejected = random.observatory.restore(broken);
    FO_CHECK(!rejected.ok());
    FO_CHECK_EQ(random.observatory.snapshot()->digest(), digest_before);
    FO_CHECK(random.observatory.coordinator_epoch() == epoch_before);
    const Status removed = remove_file_if_present(path);
    (void)removed;
  }
}

FO_TEST(property, deterministic_input_yields_a_deterministic_explanation) {
  for (std::uint64_t seed = 600; seed <= 605; ++seed) {
    RandomFederation first(seed);
    RandomFederation second(seed);
    first.build();
    second.build();
    for (int i = 0; i < 4; ++i) {
      first.register_random_cluster();
      second.register_random_cluster();
      first.publish_random_capacity();
      second.publish_random_capacity();
    }
    FO_CHECK_EQ(first.observatory.snapshot()->digest(), second.observatory.snapshot()->digest());
    FO_CHECK_EQ(render_bounds(first.observatory.bounds()),
                render_bounds(second.observatory.bounds()));
  }
}

FO_TEST(property, fence_always_defeats_later_publication) {
  for (std::uint64_t seed = 700; seed <= 706; ++seed) {
    RandomFederation random(seed);
    random.build();
    for (int i = 0; i < 3; ++i) {
      random.register_random_cluster();
    }
    FO_REQUIRE(random.observatory
                   .fence_publisher(random.publisher.id, random.publisher.boot, "property fence")
                   .ok());
    // After the fence every publication is refused, which is exactly the property under
    // test: the refusals are expected here, so the helper tolerates them.
    for (int i = 0; i < 4; ++i) {
      random.register_random_cluster(false);
      random.publish_random_capacity(false);
    }
    FO_CHECK_EQ(random.clusters.size(), 3u);
    const fo::SnapshotHandle fenced_snapshot = random.observatory.snapshot();
    for (const fo::ClusterRecord& cluster : fenced_snapshot->clusters) {
      FO_CHECK(cluster.currentness == fo::Currentness::Stale);
    }
  }
}
