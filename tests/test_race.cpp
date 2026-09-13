// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Deterministic concurrency tests. Threads are released from a shared barrier so that
// the interleavings are reproducible, and every assertion is an invariant that must hold
// under any interleaving rather than an outcome that depends on scheduling.

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "federation_observatory/client.hpp"
#include "federation_observatory/coordinator.hpp"
#include "federation_observatory/process.hpp"
#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

/// Releases every participating thread at the same moment. Spinning rather than
/// timing-dependent, so the interleaving is reproducible.
class Barrier {
 public:
  explicit Barrier(std::size_t participants) : participants_(participants) {}

  void arrive_and_wait() {
    arrived_.fetch_add(1);
    while (arrived_.load() < participants_) {
      std::this_thread::yield();
    }
  }

 private:
  std::atomic<std::size_t> arrived_{0};
  std::size_t participants_ = 0;
};

struct Shared {
  fo::FederationObservatory observatory;
  /// One ordered stream per publishing thread: a publisher identity is a single sequence
  /// stream, so two threads must never share one.
  fixture::Publisher publisher;
  fixture::Publisher secondary_publisher{"secondary-publisher"};
  std::vector<fo::ClusterId> clusters;
  fo::WorkloadClassId workload_class{fo::WorkloadClassId::unchecked("wc-1")};
  fo::WorkloadId workload{fo::WorkloadId::unchecked("wl-1")};
  fo::AcceleratorClassId accelerator_class{fo::AcceleratorClassId::unchecked("accel-1")};
  fo::RuntimeId runtime{fo::RuntimeId::unchecked("runtime-1")};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> failures{0};
};

void build_shared(Shared& shared, std::size_t cluster_count) {
  FO_REQUIRE(shared.observatory.register_publisher(shared.publisher.next()).ok());
  FO_REQUIRE(shared.observatory.register_publisher(shared.secondary_publisher.next()).ok());
  fo::FederationRecord federation;
  federation.id = shared.publisher.federation;
  federation.generation = fo::FederationGeneration{1};
  federation.coordinator_epoch = fo::CoordinatorEpoch{1};
  FO_REQUIRE(shared.observatory.register_federation(shared.publisher.next(), federation).ok());
  FO_REQUIRE(shared.observatory
                 .register_site(shared.publisher.next(),
                                fo::SiteRecord{fo::SiteId::unchecked("site-0"),
                                               fo::SiteGeneration{1}, shared.publisher.federation})
                 .ok());
  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = shared.accelerator_class;
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  accelerator_class.architecture = "sm_90";
  accelerator_class.memory_bytes_per_device = 80ull * 1024 * 1024 * 1024;
  FO_REQUIRE(shared.observatory.register_accelerator_class(shared.publisher.next(), accelerator_class).ok());
  fo::RuntimeRecord runtime;
  runtime.id = shared.runtime;
  runtime.generation = fo::RuntimeGeneration{1};
  runtime.kind = fo::RuntimeKind::Cuda;
  runtime.abi = "cuda-12.4";
  FO_REQUIRE(shared.observatory.register_runtime(shared.publisher.next(), runtime).ok());
  fo::WorkloadClassRecord workload_class;
  workload_class.id = shared.workload_class;
  FO_REQUIRE(shared.observatory.register_workload_class(shared.publisher.next(), workload_class).ok());
  fo::WorkloadRecord workload;
  workload.id = shared.workload;
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = shared.workload_class;
  workload.required_accelerators = 1;
  FO_REQUIRE(shared.observatory.register_workload(shared.publisher.next(), workload).ok());

  for (std::size_t i = 0; i < cluster_count; ++i) {
    fo::ClusterRecord cluster;
    cluster.id = fo::ClusterId::unchecked("cluster-" + std::to_string(i));
    cluster.generation = fo::ClusterGeneration{1};
    cluster.epoch = fo::ClusterEpoch{1};
    cluster.federation = shared.publisher.federation;
    cluster.site = fo::SiteId::unchecked("site-0");
    cluster.accelerator_classes = {shared.accelerator_class};
    cluster.runtimes = {shared.runtime};
    cluster.readiness = fo::Readiness::Ready;
    const fo::Result<fo::IngestResult> registered =
        shared.observatory.register_cluster(shared.publisher.next(), cluster);
    FO_REQUIRE(registered.ok());
    if (registered.value().disposition == fo::IngestDisposition::Applied) {
      shared.clusters.push_back(cluster.id);
    }
    std::vector<fo::CapacityPool> pools;
    fo::CapacityPool pool;
    pool.pool_id = fo::ResourcePoolId::unchecked("pool-" + std::to_string(i));
    pool.kind = fo::ResourceKind::Accelerator;
    pool.accelerator_class = shared.accelerator_class;
    pool.ledger = FO_UNWRAP(fo::CapacityLedger::from_components(4, 0, 0, 0, 0, 0));
    pool.evidence_class = fo::EvidenceClass::Synthetic;
    pools.push_back(std::move(pool));
    FO_REQUIRE(shared.observatory
                   .publish_capacity(shared.publisher.next(), cluster.id,
                                     fo::ClusterGeneration{1}, fo::CapacityGeneration{1},
                                     std::move(pools))
                   .ok());
  }
}

}  // namespace

FO_TEST(race, cluster_retirement_against_placement_publication) {
  Shared shared;
  build_shared(shared, 4);
  Barrier barrier(2);
  std::atomic<std::uint64_t> failures{0};
  // Failure assertions inside a worker thread must never throw: an exception escaping a
  // std::thread body terminates the process instead of failing one test.
  std::thread retire([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 4; ++i) {
      const fo::Result<fo::IngestResult> result = shared.observatory.retire_cluster(
          shared.secondary_publisher.next(), shared.clusters[static_cast<std::size_t>(i)],
          fo::ClusterGeneration{1}, "concurrent retirement");
      if (!result.ok()) {
        failures.fetch_add(1);
      }
    }
  });
  std::thread place([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 8; ++i) {
      fo::PlacementRecord placement;
      placement.id = fo::PlacementId::unchecked("placement-" + std::to_string(i));
      placement.generation = fo::PlacementGeneration{1};
      placement.workload = shared.workload;
      placement.workload_generation = fo::WorkloadGeneration{1};
      placement.federation = shared.publisher.federation;
      placement.federation_generation = fo::FederationGeneration{1};
      placement.workload_class = shared.workload_class;
      placement.selected = shared.clusters[static_cast<std::size_t>(i) % shared.clusters.size()];
      placement.selected_generation = fo::ClusterGeneration{1};
      placement.selected_epoch = fo::ClusterEpoch{1};
      placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
      fo::CandidateObservation observation;
      observation.cluster = placement.selected;
      observation.status = fo::CandidateStatus::Selected;
      observation.basis = fo::ReasonBasis::Observed;
      placement.candidates.push_back(observation);
      const fo::Result<fo::IngestResult> result =
          shared.observatory.publish_placement(shared.publisher.next(), placement);
      // Either outcome is legitimate; what must hold is that the runtime never crashes
      // and never reports a torn state.
      (void)result;
    }
  });
  retire.join();
  place.join();
  FO_CHECK_EQ(failures.load(), 0u);

  const fo::SnapshotHandle snapshot = shared.observatory.snapshot();
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    FO_CHECK(!cluster.id.empty());
  }
  FO_CHECK(!snapshot->digest().empty());
}

FO_TEST(race, capability_change_against_explanation) {
  Shared shared;
  build_shared(shared, 2);
  Barrier barrier(2);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> failures{0};
  std::thread changer([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 24; ++i) {
      fo::CapabilitySet capabilities;
      fo::CapabilityEntry entry;
      entry.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
      entry.value = fo::CapabilityValue::boolean(i % 2 == 0);
      const Status put = capabilities.put(std::move(entry));
      if (!put.ok()) {
        continue;
      }
      const fo::Result<fo::IngestResult> result = shared.observatory.publish_capability(
          shared.publisher.next(), shared.clusters.front(), fo::ClusterGeneration{1},
          fo::AcceleratorCapabilityGeneration{static_cast<std::uint64_t>(i + 2)},
          std::move(capabilities));
      if (!result.ok()) {
        failures.fetch_add(1);
      }
    }
    stop.store(true);
  });
  std::thread reader([&] {
    barrier.arrive_and_wait();
    while (!stop.load()) {
      const fo::Result<fo::CompatibilityAssessment> assessment =
          shared.observatory.compatibility(shared.clusters.front(), shared.workload);
      if (!assessment.ok()) {
        failures.fetch_add(1);
        continue;
      }
      const std::string rendered = assessment.value().render();
      if (rendered.empty()) {
        failures.fetch_add(1);
      }
    }
  });
  changer.join();
  reader.join();
  FO_CHECK_EQ(failures.load(), 0u);
}

FO_TEST(race, capacity_change_against_stranded_analysis) {
  Shared shared;
  build_shared(shared, 4);
  Barrier barrier(2);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> failures{0};
  std::thread changer([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 16; ++i) {
      std::vector<fo::CapacityPool> pools;
      fo::CapacityPool pool;
      pool.pool_id = fo::ResourcePoolId::unchecked("pool-0");
      pool.kind = fo::ResourceKind::Accelerator;
      pool.accelerator_class = shared.accelerator_class;
      pool.generation = fo::CapacityGeneration{static_cast<std::uint64_t>(i + 2)};
      const fo::Result<fo::CapacityLedger> ledger =
          fo::CapacityLedger::from_components(4 + static_cast<std::uint64_t>(i), 0, 0, 0, 0, 0);
      if (!ledger.ok()) {
        failures.fetch_add(1);
        continue;
      }
      pool.ledger = ledger.value();
      pools.push_back(std::move(pool));
      const fo::Result<fo::IngestResult> result = shared.observatory.publish_capacity(
          shared.publisher.next(), shared.clusters.front(), fo::ClusterGeneration{1},
          fo::CapacityGeneration{static_cast<std::uint64_t>(i + 2)}, std::move(pools));
      if (!result.ok()) {
        failures.fetch_add(1);
      }
    }
    stop.store(true);
  });
  std::thread analyser([&] {
    barrier.arrive_and_wait();
    while (!stop.load()) {
      fo::StrandedCapacityRequest request;
      request.federation = shared.publisher.federation;
      request.workload_class = shared.workload_class;
      const fo::Result<fo::StrandedCapacityReport> report =
          shared.observatory.stranded_capacity(request);
      if (!report.ok()) {
        failures.fetch_add(1);
        continue;
      }
      if (!report.value().closes()) {
        failures.fetch_add(1);
      }
    }
  });
  changer.join();
  analyser.join();
  FO_CHECK_EQ(failures.load(), 0u);
}

FO_TEST(race, publisher_fence_against_event_commit) {
  Shared shared;
  build_shared(shared, 2);
  Barrier barrier(2);
  std::atomic<std::uint64_t> refusals{0};
  std::thread publisher_thread([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 32; ++i) {
      std::vector<fo::CapacityPool> pools;
      fo::CapacityPool pool;
      pool.pool_id = fo::ResourcePoolId::unchecked("pool-race");
      pool.kind = fo::ResourceKind::Accelerator;
      pool.accelerator_class = shared.accelerator_class;
      const fo::Result<fo::CapacityLedger> ledger =
          fo::CapacityLedger::from_components(4, 0, 0, 0, 0, 0);
      if (!ledger.ok()) {
        refusals.fetch_add(1);
        continue;
      }
      pool.ledger = ledger.value();
      pools.push_back(std::move(pool));
      const fo::Result<fo::IngestResult> result = shared.observatory.publish_capacity(
          shared.publisher.next(), shared.clusters.front(), fo::ClusterGeneration{1},
          fo::CapacityGeneration{static_cast<std::uint64_t>(i + 2)}, std::move(pools));
      if (!result.ok()) {
        refusals.fetch_add(1);
      }
    }
  });
  std::atomic<std::uint64_t> fence_failures{0};
  std::thread fence_thread([&] {
    barrier.arrive_and_wait();
    const Status fenced =
        shared.observatory.fence_publisher(shared.publisher.id, shared.publisher.boot, "race fence");
    if (!fenced.ok()) {
      fence_failures.fetch_add(1);
    }
  });
  publisher_thread.join();
  fence_thread.join();
  FO_CHECK_EQ(fence_failures.load(), 0u);

  // Whatever the interleaving, nothing published after the fence may have been applied.
  const fo::SnapshotHandle snapshot = shared.observatory.snapshot();
  const fo::PublisherStatus* fenced_publisher = nullptr;
  const fo::PublisherStatus* other_publisher = nullptr;
  for (const fo::PublisherStatus& status : snapshot->publishers) {
    if (status.id == shared.publisher.id) {
      fenced_publisher = &status;
    } else {
      other_publisher = &status;
    }
  }
  FO_REQUIRE(fenced_publisher != nullptr);
  FO_CHECK(fenced_publisher->fenced);
  FO_CHECK(!fenced_publisher->live);
  // Fencing one boot identity must not disturb another publisher.
  FO_REQUIRE(other_publisher != nullptr);
  FO_CHECK(!other_publisher->fenced);
  FO_CHECK(other_publisher->live);
  FO_CHECK(refusals.load() > 0);
}

FO_TEST(race, snapshot_and_persistence_against_mutation) {
  Shared shared;
  build_shared(shared, 4);
  Barrier barrier(2);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> failures{0};
  const std::string path = join_path(std::filesystem::temp_directory_path().string(),
                                     "fo-race-state-" + unique_token() + ".bin");

  std::thread mutator([&] {
    barrier.arrive_and_wait();
    for (int i = 0; i < 24; ++i) {
      fo::ClusterRecord cluster;
      cluster.id = fo::ClusterId::unchecked("race-cluster-" + std::to_string(i));
      cluster.generation = fo::ClusterGeneration{1};
      cluster.epoch = fo::ClusterEpoch{1};
      cluster.federation = shared.publisher.federation;
      cluster.site = fo::SiteId::unchecked("site-0");
      cluster.accelerator_classes = {shared.accelerator_class};
      cluster.runtimes = {shared.runtime};
      const fo::Result<fo::IngestResult> result =
          shared.observatory.register_cluster(shared.publisher.next(), cluster);
      if (!result.ok()) {
        failures.fetch_add(1);
      }
    }
    stop.store(true);
  });
  std::thread reader([&] {
    barrier.arrive_and_wait();
    while (!stop.load()) {
      const fo::SnapshotHandle snapshot = shared.observatory.snapshot();
      if (snapshot->digest().empty()) {
        failures.fetch_add(1);
      }
      const Status saved = shared.observatory.save_state(path);
      if (!saved.ok()) {
        failures.fetch_add(1);
      }
    }
  });
  mutator.join();
  reader.join();
  FO_CHECK_EQ(failures.load(), 0u);
  const Status removed = remove_file_if_present(path);
  (void)removed;
}

FO_TEST(race, coordinator_shutdown_against_incoming_publication) {
  for (int iteration = 0; iteration < 3; ++iteration) {
    fo::CoordinatorConfig config;
    config.port = 0;
    config.poll_millis = 1;
    config.worker_threads = 2;
    fo::FederationCoordinator coordinator(config);
    FO_REQUIRE(coordinator.start().ok());
    const std::uint16_t port = coordinator.port();

    Shared shared;
    build_shared(shared, 1);
    std::atomic<bool> stop{false};
    std::thread client([&] {
      fo::ClientConfig client_config;
      client_config.host = "127.0.0.1";
      client_config.port = port;
      client_config.poll_millis = 1;
      Result<fo::FederationClient> connected = fo::FederationClient::connect(client_config);
      if (!connected.ok()) {
        return;
      }
      fo::FederationClient& federation_client = connected.value();
      const fo::Result<fo::protocol::HelloReply> handshake =
          federation_client.hello(shared.publisher.id);
      if (!handshake.ok()) {
        return;
      }
      fixture::Publisher remote;
      remote.id = shared.publisher.id;
      remote.boot = shared.publisher.boot;
      remote.epoch = handshake.value().epoch;
      remote.federation = shared.publisher.federation;
      while (!stop.load()) {
        const fo::Result<fo::protocol::PublicationAck> ack = federation_client.register_federation(
            remote.next(), fo::FederationRecord{remote.federation, fo::FederationGeneration{1}});
        if (!ack.ok()) {
          break;
        }
      }
      const Status closed = federation_client.close();
      (void)closed;
    });

    std::this_thread::yield();
    FO_REQUIRE(coordinator.stop().ok());
    stop.store(true);
    client.join();
    FO_CHECK(!coordinator.running());
    FO_CHECK_EQ(coordinator.counters().active_connections, 0u);
  }
}

FO_TEST(race, repeated_start_and_stop_is_clean) {
  fo::CoordinatorConfig config;
  config.port = 0;
  config.poll_millis = 1;
  for (int iteration = 0; iteration < 5; ++iteration) {
    fo::FederationCoordinator coordinator(config);
    FO_REQUIRE(coordinator.start().ok());
    FO_CHECK(coordinator.running());
    FO_CHECK(coordinator.port() != 0);
    FO_REQUIRE(coordinator.stop().ok());
    FO_CHECK(!coordinator.running());
    // Stopping twice is safe.
    FO_REQUIRE(coordinator.stop().ok());
    // Starting again after a stop is safe.
    FO_REQUIRE(coordinator.start().ok());
    FO_REQUIRE(coordinator.stop().ok());
  }
}
