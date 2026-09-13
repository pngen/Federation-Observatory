// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_publisher_fencing - fencing a boot identity and what it takes to become current again.
//
// What this proves, in order:
//   * while the publisher boot identity is live, its cluster evidence is CURRENT;
//   * fencing that boot identity permanently revokes its authority, and every record it
//     produced stops being current - the snapshot says STALE, not "unchanged";
//   * a replayed publication from the fenced identity is REFUSED (disposition FENCED,
//     error code FencedPublisher) instead of being applied;
//   * a replacement process presenting a fresh boot identity can register, but
//     registration alone restores nothing: the cluster evidence stays STALE until the new
//     identity republishes it, and even a byte-identical replay under the same generation
//     is suppressed as a duplicate rather than silently restoring currentness.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic) and is
// NOT a physical federation. Fencing is an observation-side authority decision about
// evidence, not a runtime command to the upstream scheduler.

#include <cstdio>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/snapshot.hpp"
#include "federation_observatory/synthetic.hpp"
#include "federation_observatory/version.hpp"

namespace {

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

struct Scenario {
  fo::FederationObservatory observatory;
  fo::SyntheticConfig config;
  fo::ObservatorySink sink;
  fo::SyntheticFederation federation;

  Scenario()
      : observatory(fo::ObservatoryConfig{}),
        sink(observatory, config.publisher, config.boot, config.federation),
        federation(config, sink) {}
};

std::size_t count_currentness(const fo::SnapshotHandle& snapshot, fo::Currentness wanted) {
  std::size_t count = 0;
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    if (cluster.currentness == wanted) {
      ++count;
    }
  }
  return count;
}

fo::Currentness cluster_currentness(const fo::SnapshotHandle& snapshot, const char* cluster) {
  const fo::ClusterRecord* record = snapshot->find_cluster(fo::ClusterId::unchecked(cluster));
  return record == nullptr ? fo::Currentness::Unknown : record->currentness;
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: publisher fencing\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example publishes a SYNTHETIC federation, fences the publisher boot identity,\n"
      "shows the evidence going stale, shows a replay being refused, and shows what a fresh\n"
      "boot identity must do before evidence is current again.\n");

  Scenario scenario;
  const fo::PublisherId publisher = scenario.config.publisher;
  const fo::BootGeneration original_boot = scenario.config.boot;
  const fo::Status registered = scenario.sink.register_self();
  if (!registered.ok()) {
    return fail(registered, "register publisher");
  }
  const fo::Status published = scenario.federation.run_all();
  if (!published.ok()) {
    return fail(published, "publish synthetic federation");
  }

  section("before fencing");
  {
    const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
    std::printf("  publisher %s boot %s live: %s\n", publisher.value().c_str(),
                original_boot.to_string().c_str(),
                scenario.observatory.publisher_is_live(publisher, original_boot) ? "yes" : "no");
    std::printf("  clusters CURRENT: %zu of %zu\n",
                count_currentness(snapshot, fo::Currentness::Current), snapshot->clusters.size());
    std::printf("  cluster-0-0 currentness: %s\n",
                std::string(fo::to_string(cluster_currentness(snapshot, "cluster-0-0"))).c_str());
  }

  const fo::SnapshotHandle before = scenario.observatory.snapshot();
  const fo::ClusterRecord* target = before->find_cluster(fo::ClusterId::unchecked("cluster-0-0"));
  if (target == nullptr) {
    std::printf("FAILED [snapshot]: cluster-0-0 is missing\n");
    return 1;
  }
  const fo::CapabilitySet published_capabilities = target->capabilities;
  const std::vector<fo::CapacityPool> published_pools = target->capacity_pools;

  section("fencing the publisher boot identity");
  {
    const fo::Status fenced = scenario.observatory.fence_publisher(
        publisher, original_boot, "synthetic example: the publishing process was replaced");
    if (!fenced.ok()) {
      return fail(fenced, "fence publisher");
    }
    std::printf("  fence status: %s\n", fenced.to_string().c_str());
    std::printf("  publisher_is_live: %s\n",
                scenario.observatory.publisher_is_live(publisher, original_boot) ? "yes" : "no");
  }

  section("evidence produced by the fenced identity is no longer current");
  {
    const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
    std::printf("  clusters CURRENT: %zu, STALE: %zu of %zu\n",
                count_currentness(snapshot, fo::Currentness::Current),
                count_currentness(snapshot, fo::Currentness::Stale), snapshot->clusters.size());
    std::printf("  cluster-0-0 currentness: %s\n",
                std::string(fo::to_string(cluster_currentness(snapshot, "cluster-0-0"))).c_str());
    std::printf("  snapshot degraded: %s\n", snapshot->health.degraded ? "yes" : "no");
  }

  section("a replay from the fenced identity is refused");
  {
    const fo::Status replay = scenario.sink.emit_capability(
        fo::ClusterId::unchecked("cluster-0-0"), fo::ClusterGeneration{1},
        fo::AcceleratorCapabilityGeneration{1}, published_capabilities);
    std::printf("  replay status: %s\n", replay.to_string().c_str());
    if (replay.ok()) {
      std::printf("FAILED [fencing]: a publication from the fenced identity was accepted\n");
      return 1;
    }
    if (replay.code() != fo::ErrorCode::FencedPublisher) {
      std::printf("FAILED [fencing]: refusal was classified as %s, expected FENCED_PUBLISHER\n",
                  std::string(fo::to_string(replay.code())).c_str());
      return 1;
    }
  }

  section("a fresh boot identity registers, but restores nothing by itself");
  const fo::BootGeneration replacement_boot = original_boot.next();
  fo::ObservatorySink replacement(scenario.observatory, publisher, replacement_boot,
                                  scenario.config.federation);
  {
    const fo::Status fresh = replacement.register_self();
    if (!fresh.ok()) {
      return fail(fresh, "register the replacement boot identity");
    }
    const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
    std::printf("  publisher %s boot %s live: %s\n", publisher.value().c_str(),
                replacement_boot.to_string().c_str(),
                scenario.observatory.publisher_is_live(publisher, replacement_boot) ? "yes" : "no");
    std::printf("  clusters CURRENT after registration: %zu of %zu\n",
                count_currentness(snapshot, fo::Currentness::Current), snapshot->clusters.size());
    std::printf("  cluster-0-0 currentness: %s\n",
                std::string(fo::to_string(cluster_currentness(snapshot, "cluster-0-0"))).c_str());
    if (count_currentness(snapshot, fo::Currentness::Current) != 0) {
      std::printf("FAILED [registration]: registering a boot identity restored evidence by itself\n");
      return 1;
    }
  }

  section("the replacement identity must republish");
  {
    const fo::Status identical = replacement.emit_capability(
        fo::ClusterId::unchecked("cluster-0-0"), fo::ClusterGeneration{1},
        fo::AcceleratorCapabilityGeneration{1}, published_capabilities);
    std::printf("  byte-identical republication under generation 1: %s\n", identical.to_string().c_str());
    std::printf("  cluster-0-0 currentness: %s\n",
                std::string(fo::to_string(cluster_currentness(scenario.observatory.snapshot(),
                                                              "cluster-0-0")))
                    .c_str());
  }
  {
    const fo::Status capability = replacement.emit_capability(
        fo::ClusterId::unchecked("cluster-0-0"), fo::ClusterGeneration{1},
        fo::AcceleratorCapabilityGeneration{2}, published_capabilities);
    if (!capability.ok()) {
      return fail(capability, "republish capability evidence");
    }
    const fo::Status capacity = replacement.emit_capacity(
        fo::ClusterId::unchecked("cluster-0-0"), fo::ClusterGeneration{1},
        fo::CapacityGeneration{2}, published_pools);
    if (!capacity.ok()) {
      return fail(capacity, "republish capacity evidence");
    }
  }

  section("after the replacement identity republished");
  {
    const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
    std::printf("  clusters CURRENT: %zu, STALE: %zu of %zu\n",
                count_currentness(snapshot, fo::Currentness::Current),
                count_currentness(snapshot, fo::Currentness::Stale), snapshot->clusters.size());
    std::printf("  cluster-0-0 currentness: %s\n",
                std::string(fo::to_string(cluster_currentness(snapshot, "cluster-0-0"))).c_str());
    std::printf("  cluster-0-0 capability_generation: %s\n",
                snapshot->find_cluster(fo::ClusterId::unchecked("cluster-0-0"))
                    ->capability_generation.to_string()
                    .c_str());
    std::printf("  publishers: %zu total, %zu live, %zu fenced\n", snapshot->health.publishers_total,
                snapshot->health.publishers_live, snapshot->health.publishers_fenced);
    if (cluster_currentness(snapshot, "cluster-0-0") != fo::Currentness::Current) {
      std::printf("FAILED [revalidation]: the republished cluster evidence is not current\n");
      return 1;
    }
    if (count_currentness(snapshot, fo::Currentness::Current) != 1) {
      std::printf("FAILED [revalidation]: republication restored more than the republished cluster\n");
      return 1;
    }
  }

  section("what fencing means");
  std::printf(
      "  Fencing is permanent for a boot identity: the fenced identity can never publish\n"
      "  again, and every record it produced is historical. Currentness is restored only by\n"
      "  fresh evidence published under a new boot identity and a new generation. The upstream\n"
      "  scheduler's own decisions are untouched by any of this: the runtime revoked its trust\n"
      "  in evidence, not in the scheduler.\n");
  return 0;
}
