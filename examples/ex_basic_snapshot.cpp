// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_basic_snapshot - take a snapshot of a published federation and read it back.
//
// What this proves:
//   * the synthetic backend can be driven in-process through the public publication API
//     and the runtime accepts every ordered step;
//   * a snapshot is an immutable value that exposes membership (federation, sites,
//     clusters, classes, runtimes, workloads, domains, placements, migrations);
//   * per-cluster currentness is visible on the snapshot, and the freshness census
//     (SnapshotHealth) is reported next to the data instead of being assumed.
//
// The scenario is SYNTHETIC. It is produced by fo::SyntheticFederation, every record
// carries EvidenceClass::Synthetic, and it is NOT a physical multi-cluster federation.

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

template <class Id>
std::string join_ids(const std::vector<Id>& ids) {
  std::string out;
  for (const Id& id : ids) {
    if (!out.empty()) {
      out += ",";
    }
    out += fo::render_id(id.value());
  }
  return out.empty() ? std::string("-") : out;
}

std::string generation_text(const fo::ClusterGeneration& generation) {
  return generation.is_set() ? generation.to_string() : std::string("-");
}

/// The synthetic scenario and the observatory that receives it. Declaration order keeps
/// the sink bound to the observatory and the federation bound to the sink.
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

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: basic snapshot\n",
              std::string(fo::version_string()).c_str());
  std::printf("%s\n", std::string(fo::build_banner()).c_str());
  std::printf(
      "This example publishes a SYNTHETIC federation in-process, snapshots it, and prints\n"
      "membership, per-cluster currentness and the snapshot freshness census. No physical\n"
      "federation is involved and no placement decision is attributed to this runtime.\n");

  Scenario scenario;
  const fo::Status registered = scenario.sink.register_self();
  if (!registered.ok()) {
    return fail(registered, "register publisher");
  }
  const fo::Status published = scenario.federation.run_all();
  if (!published.ok()) {
    return fail(published, "publish synthetic federation");
  }

  const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
  if (snapshot == nullptr) {
    std::printf("FAILED [snapshot]: the runtime returned no snapshot\n");
    return 1;
  }

  section("publication");
  std::printf("  publisher                    %s\n", scenario.config.publisher.value().c_str());
  std::printf("  boot                         %s\n", scenario.config.boot.to_string().c_str());
  std::printf("  federation                   %s\n", scenario.config.federation.value().c_str());
  std::printf("  evidence_class               %s\n",
              std::string(fo::to_string(snapshot->evidence_class)).c_str());
  std::printf("  step vocabulary              %zu synthetic steps; run_all published the ordered subset\n",
              fo::kSyntheticStepCount);

  section("membership");
  std::printf("  federation                   %s generation %s (%s)\n",
              fo::render_id(snapshot->federation.value()).c_str(),
              snapshot->generation.is_set() ? snapshot->generation.to_string().c_str() : "-",
              snapshot->federations.empty()
                  ? "-"
                  : snapshot->federations.front().display_name.c_str());
  std::printf("  sites                        %zu\n", snapshot->sites.size());
  std::printf("  clusters                     %zu\n", snapshot->clusters.size());
  std::printf("  accelerator_classes          %zu\n", snapshot->accelerator_classes.size());
  std::printf("  runtimes                     %zu\n", snapshot->runtimes.size());
  std::printf("  backends                     %zu\n", snapshot->backends.size());
  std::printf("  domains                      %zu\n", snapshot->domains.size());
  std::printf("  policies                     %zu\n", snapshot->policies.size());
  std::printf("  artifacts                    %zu\n", snapshot->artifacts.size());
  std::printf("  workload_classes             %zu\n", snapshot->workload_classes.size());
  std::printf("  workloads                    %zu\n", snapshot->workloads.size());
  std::printf("  placements                   %zu\n", snapshot->placements.size());
  std::printf("  migrations                   %zu\n", snapshot->migrations.size());

  const std::size_t expected_clusters = scenario.federation.cluster_ids().size();
  if (snapshot->clusters.size() != expected_clusters) {
    std::printf("FAILED [membership]: snapshot holds %zu clusters but %zu were published\n",
                snapshot->clusters.size(), expected_clusters);
    return 1;
  }

  section("sites");
  {
    std::vector<std::vector<std::string>> rows;
    for (const fo::SiteRecord& site : snapshot->sites) {
      rows.push_back({site.id.value(), site.region, site.zone, site.failure_domain,
                      std::to_string(site.clusters.size()),
                      std::string(fo::to_string(site.currentness))});
    }
    std::printf("%s\n", fo::render_table({"site", "region", "zone", "failure_domain", "clusters",
                                        "currentness"},
                                       rows, "  ")
                           .c_str());
  }

  section("clusters and their currentness");
  {
    std::vector<std::vector<std::string>> rows;
    std::size_t current = 0;
    for (const fo::ClusterRecord& cluster : snapshot->clusters) {
      if (cluster.currentness == fo::Currentness::Current) {
        ++current;
      }
      rows.push_back({cluster.id.value(), fo::render_id(cluster.site.value()),
                      fo::render_id(cluster.domain.value()), join_ids(cluster.accelerator_classes),
                      join_ids(cluster.runtimes), generation_text(cluster.generation),
                      cluster.epoch.is_set() ? cluster.epoch.to_string() : std::string("-"),
                      std::string(fo::to_string(cluster.readiness)),
                      std::string(fo::to_string(cluster.currentness))});
    }
    std::printf("%s\n", fo::render_table({"cluster", "site", "domain", "accelerator_class", "runtime",
                                        "generation", "epoch", "readiness", "currentness"},
                                       rows, "  ")
                           .c_str());
    std::printf("  clusters reporting CURRENT: %zu of %zu\n", current, snapshot->clusters.size());
  }

  section("publishers");
  {
    std::vector<std::vector<std::string>> rows;
    for (const fo::PublisherStatus& publisher : snapshot->publishers) {
      rows.push_back({publisher.id.value(),
                      publisher.boot.is_set() ? publisher.boot.to_string() : std::string("-"),
                      fo::render_id(publisher.federation.value()),
                      publisher.live ? "yes" : "no", publisher.fenced ? "yes" : "no",
                      publisher.watermark.to_string(),
                      std::to_string(publisher.publications_accepted),
                      std::to_string(publisher.publications_rejected)});
    }
    std::printf("%s\n", fo::render_table(
                           {"publisher", "boot", "federation", "live", "fenced", "watermark",
                            "accepted", "rejected"},
                           rows, "  ")
                           .c_str());
  }

  section("snapshot health");
  std::printf("%s\n", snapshot->health.render("  ").c_str());
  if (snapshot->health.degraded) {
    std::printf("  NOTE: at least one dynamic record is not CURRENT; consumers must render this.\n");
  }

  section("snapshot integrity");
  std::printf("  coordinator_epoch            %s\n", snapshot->coordinator_epoch.to_string().c_str());
  std::printf("  snapshot_generation          %s\n",
              snapshot->snapshot_generation.to_string().c_str());
  std::printf("  records_included             %zu\n", snapshot->records_included);
  std::printf("  records_total                %zu\n", snapshot->records_total);
  std::printf("  truncated                    %s\n", snapshot->truncated ? "yes" : "no");
  std::printf("  precision                    %s\n",
              std::string(fo::to_string(snapshot->precision)).c_str());
  std::printf("  publishers_in_snapshot       %zu\n", snapshot->publishers.size());

  section("attribution");
  std::printf(
      "  Federation Observatory observed the records above; it published nothing and chose\n"
      "  no placement. Placement and migration decisions belong to the upstream scheduler.\n");
  return 0;
}
