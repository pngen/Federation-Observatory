// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_stranded_capacity - nominal vs usable vs stranded vs unknown, exactly accounted.
//
// What this proves:
//   * the capacity decomposition is a checked identity. The ledger satisfies
//     nominal = offline + allocated + reserved + draining + unusable + idle, and the
//     eligibility partition satisfies idle = usable + unknown + stranded;
//   * stranded capacity is reported per reason, with technical and policy causes kept
//     distinct, and every unit carries exactly one primary reason so amounts sum without
//     double counting;
//   * raising a cluster's accelerator capability generation changes what the published
//     evidence supports, so a finding computed under the previous generation is no longer
//     current and must be recomputed - both are printed here.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic). The
// numbers describe generated records, NOT a physical fleet, and the runtime neither
// allocated nor stranded anything in reality: it observed and explained.

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

/// Whether the cluster's own published capability set declares fp8 support.
bool declares(const fo::ClusterRecord* cluster) {
  fo::CapabilityRef reference;
  reference.key = fo::CapabilityKey::PrecisionFp8E4M3;
  return cluster != nullptr && cluster->capabilities.contains(reference);
}

const fo::StrandedCapacityFinding* find_finding(const fo::StrandedCapacityReport& report,
                                                const char* cluster,
                                                const char* accelerator_class) {
  for (const fo::StrandedCapacityFinding& finding : report.findings) {
    if (finding.cluster.value() == cluster && finding.accelerator_class.value() == accelerator_class) {
      return &finding;
    }
  }
  return nullptr;
}

void print_reasons(const fo::ReasonTally& tally) {
  const std::vector<std::pair<fo::StrandingReason, std::uint64_t>> reasons = tally.non_zero();
  if (reasons.empty()) {
    std::printf("  <no capacity was stranded>\n");
    return;
  }
  std::vector<std::vector<std::string>> rows;
  for (const auto& entry : reasons) {
    rows.push_back({std::string(fo::to_string(entry.first)), std::to_string(entry.second),
                    fo::is_technical_reason(entry.first) ? "technical" : "policy"});
  }
  std::printf("%s\n", fo::render_table({"stranding_reason", "amount", "nature"}, rows, "  ").c_str());
}

fo::Result<fo::StrandedCapacityReport> stranded(const Scenario& scenario,
                                                const fo::WorkloadClassId& workload_class) {
  fo::StrandedCapacityRequest request;
  request.federation = scenario.config.federation;
  request.workload_class = workload_class;
  request.kind = fo::ResourceKind::Accelerator;
  request.include_stale = false;
  return scenario.observatory.stranded_capacity(request);
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: stranded capacity\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example prints the exact checked accounting of a SYNTHETIC fleet for one\n"
      "workload class, decomposes the stranded part by reason, then raises the accelerator\n"
      "capability generation and prints the recomputed finding next to the earlier one.\n");

  Scenario scenario;
  const fo::Status registered = scenario.sink.register_self();
  if (!registered.ok()) {
    return fail(registered, "register publisher");
  }
  const fo::Status published = scenario.federation.run_all();
  if (!published.ok()) {
    return fail(published, "publish synthetic federation");
  }

  const fo::WorkloadClassId workload_class = fo::WorkloadClassId::unchecked("wc-train");
  const fo::Result<fo::StrandedCapacityReport> before = stranded(scenario, workload_class);
  if (!before.ok()) {
    return fail(before, "stranded capacity before the capability change");
  }

  section("report and checked accounting");
  std::printf("%s\n", before.value().summary.render("  ").c_str());
  std::printf("  accounting_closes            %s\n", before.value().closes() ? "yes" : "no");
  std::printf("  excluded_stale_clusters      %zu\n", before.value().excluded_stale_clusters.size());
  std::printf("  excluded_unknown_clusters    %zu\n",
              before.value().excluded_unknown_clusters.size());
  std::printf("  digest                       %s\n", before.value().digest().c_str());
  if (!before.value().closes()) {
    std::printf("FAILED [accounting]: the stranded report does not close\n");
    return 1;
  }

  section("per pool: ledger identity, eligibility partition and reasons");
  {
    fo::ReasonTally aggregate;
    for (const fo::StrandedCapacityFinding& finding : before.value().findings) {
      std::printf("  cluster %s pool %s class %s | capacity_generation %s capability_generation %s\n",
                  finding.cluster.value().c_str(), finding.pool.value().c_str(),
                  fo::render_id(finding.accelerator_class.value()).c_str(),
                  finding.capacity_generation.to_string().c_str(),
                  finding.capability_generation.to_string().c_str());
      std::printf("%s\n", finding.ledger.render("    ").c_str());
      std::printf("%s\n", finding.breakdown.render("    ").c_str());
      std::printf("    partition_closes: %s\n",
                  finding.breakdown.closes() ? "yes" : "no");
      for (const auto& entry : finding.breakdown.stranded_by_primary_reason.non_zero()) {
        const fo::Status added = aggregate.add(entry.first, entry.second);
        if (!added.ok()) {
          return fail(added, "aggregate stranding reasons");
        }
      }
    }
    section("federation-wide reason decomposition (primary reasons, non-overlapping)");
    print_reasons(aggregate);
    std::printf("  total stranded by primary reason: %llu\n",
                static_cast<unsigned long long>(aggregate.total()));
  }

  section("capability generation change");
  const fo::SnapshotHandle before_snapshot = scenario.observatory.snapshot();
  const fo::ClusterRecord* cluster_before =
      before_snapshot->find_cluster(fo::ClusterId::unchecked("cluster-0-0"));
  if (cluster_before == nullptr) {
    std::printf("FAILED [snapshot]: cluster-0-0 is missing\n");
    return 1;
  }
  std::printf("  cluster-0-0 capability_generation before: %s\n",
              cluster_before->capability_generation.to_string().c_str());

  const fo::Status changed = scenario.federation.run(fo::SyntheticStep::CapabilityChange);
  if (!changed.ok()) {
    return fail(changed, "publish the accelerator capability change");
  }

  const fo::SnapshotHandle after_snapshot = scenario.observatory.snapshot();
  const fo::ClusterRecord* cluster_after =
      after_snapshot->find_cluster(fo::ClusterId::unchecked("cluster-0-0"));
  if (cluster_after == nullptr) {
    std::printf("FAILED [snapshot]: cluster-0-0 is missing after the change\n");
    return 1;
  }
  const fo::Result<fo::StrandedCapacityReport> after = stranded(scenario, workload_class);
  if (!after.ok()) {
    return fail(after, "stranded capacity after the capability change");
  }

  const fo::StrandedCapacityFinding* row_before =
      find_finding(before.value(), "cluster-0-0", "accel-class-0");
  const fo::StrandedCapacityFinding* row_after =
      find_finding(after.value(), "cluster-0-0", "accel-class-0");
  if (row_before == nullptr || row_after == nullptr) {
    std::printf("FAILED [findings]: the cluster-0-0 accelerator pool is absent from a report\n");
    return 1;
  }
  std::printf("  cluster-0-0 capability_generation after:  %s\n",
              cluster_after->capability_generation.to_string().c_str());

  section("the same finding, recomputed under the new generation");
  {
    std::vector<std::vector<std::string>> rows;
    rows.push_back({"capability_generation", row_before->capability_generation.to_string(),
                    row_after->capability_generation.to_string()});
    rows.push_back({"capacity_generation", row_before->capacity_generation.to_string(),
                    row_after->capacity_generation.to_string()});
    rows.push_back({"idle", std::to_string(row_before->breakdown.idle),
                    std::to_string(row_after->breakdown.idle)});
    rows.push_back({"usable", std::to_string(row_before->breakdown.usable),
                    std::to_string(row_after->breakdown.usable)});
    rows.push_back({"unknown", std::to_string(row_before->breakdown.unknown),
                    std::to_string(row_after->breakdown.unknown)});
    rows.push_back({"stranded", std::to_string(row_before->breakdown.stranded()),
                    std::to_string(row_after->breakdown.stranded())});
    rows.push_back({"cluster declares FP8_E4M3", declares(cluster_before) ? "yes" : "no",
                    declares(cluster_after) ? "yes" : "no"});
    std::printf("%s\n", fo::render_table({"field", "before change", "after change"}, rows, "  ").c_str());
    std::printf("\n  stranding reasons after the change:\n");
    print_reasons(row_after->breakdown.stranded_by_primary_reason);

    const fo::AcceleratorClassRecord* catalogue = after_snapshot->find_accelerator_class(
        fo::AcceleratorClassId::unchecked("accel-class-0"));
    fo::CapabilityRef fp8;
    fp8.key = fo::CapabilityKey::PrecisionFp8E4M3;
    const bool catalogue_declares = catalogue != nullptr && catalogue->capabilities.contains(fp8);
    if (!declares(cluster_after) && catalogue_declares) {
      std::printf(
          "  The cluster withdrew its own fp8 declaration, but the accelerator-class catalogue\n"
          "  still advertises it and the runtime evaluates the merged evidence, so this pool's\n"
          "  eligibility is unchanged. The capability generation advanced regardless, and that is\n"
          "  what makes the earlier finding historical: consumers recompute instead of assuming.\n");
    }
  }

  section("why the earlier finding is no longer current");
  std::printf(
      "  The earlier finding was computed from accelerator capability generation %s. The\n"
      "  cluster now publishes generation %s, so that finding describes evidence that has been\n"
      "  superseded: it is historical, not current, and no consumer may act on it. The runtime\n"
      "  did not invalidate the accelerator; the published evidence changed, and the finding\n"
      "  was recomputed from the new evidence rather than quietly amended.\n",
      row_before->capability_generation.to_string().c_str(),
      row_after->capability_generation.to_string().c_str());
  if (row_before->capability_generation == row_after->capability_generation) {
    std::printf("FAILED [currentness]: the capability generation did not advance\n");
    return 1;
  }
  std::printf(
      "  Federation Observatory observed and explained this; it did not change any capability\n"
      "  and does not own capacity policy.\n");
  return 0;
}
