// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_federation_fragmentation - enough capacity in aggregate, but no legal group.
//
// What this proves:
//   * fragmentation is reported as a distribution problem, not as a shortage: the
//     aggregate usable capacity covers the required co-dependent group while no single
//     legal placement domain can host it, and the classification says which it is;
//   * the per-domain table shows exactly where the usable capacity sits, so the finding
//     is auditable rather than a verdict;
//   * after the synthetic topology is repaired (a new cluster with a full group in one
//     domain) the same analysis is recomputed and the classification changes.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic). It is
// NOT a physical federation, and Federation Observatory did not place anything: it
// grouped observed usable capacity into observed domains and reported what it found.

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

fo::Result<fo::FragmentationFinding> analyse(const Scenario& scenario,
                                            const fo::WorkloadClassId& workload_class) {
  return scenario.observatory.fragmentation(scenario.config.federation, workload_class,
                                            fo::ResourceKind::Accelerator);
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: federation fragmentation\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example shows a SYNTHETIC federation whose aggregate usable capacity covers a\n"
      "co-dependent group that no single legal placement domain can host, then repairs the\n"
      "topology and recomputes the classification.\n");

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
  const fo::Result<fo::FragmentationFinding> before = analyse(scenario, workload_class);
  if (!before.ok()) {
    return fail(before, "fragmentation before the repair");
  }

  section("finding before the topology repair");
  std::printf("%s\n", before.value().render("  ").c_str());

  section("premise: aggregate capacity is sufficient, one legal domain is not");
  std::printf("  required_per_group           %llu\n",
              static_cast<unsigned long long>(before.value().required_per_group));
  std::printf("  aggregate_nominal            %llu\n",
              static_cast<unsigned long long>(before.value().aggregate_nominal));
  std::printf("  aggregate_usable             %llu\n",
              static_cast<unsigned long long>(before.value().aggregate_usable));
  std::printf("  aggregate_stranded           %llu\n",
              static_cast<unsigned long long>(before.value().aggregate_stranded));
  std::printf("  largest_legal_group          %llu (domain %s)\n",
              static_cast<unsigned long long>(before.value().largest_legal_group),
              fo::render_id(before.value().largest_domain.value()).c_str());
  std::printf("  legal_domain_count           %llu\n",
              static_cast<unsigned long long>(before.value().legal_domain_count));
  std::printf("  classification               %s\n",
              std::string(fo::to_string(before.value().classification)).c_str());
  bool premise_holds = true;
  if (before.value().aggregate_usable < before.value().required_per_group) {
    std::printf(
        "  DIAGNOSIS: aggregate usable capacity %llu is below the required group %llu, so this\n"
        "  federation is short of capacity rather than fragmented, and CAPACITY_SHORTAGE is the\n"
        "  correct classification. The scenario does not exercise the property this example\n"
        "  demonstrates; the run below still shows the repair and the recomputation.\n",
        static_cast<unsigned long long>(before.value().aggregate_usable),
        static_cast<unsigned long long>(before.value().required_per_group));
    premise_holds = false;
  } else if (before.value().largest_legal_group >= before.value().required_per_group) {
    std::printf(
        "  DIAGNOSIS: some single legal domain already holds %llu >= %llu, so the federation is\n"
        "  not fragmented for this workload class.\n",
        static_cast<unsigned long long>(before.value().largest_legal_group),
        static_cast<unsigned long long>(before.value().required_per_group));
    premise_holds = false;
  } else {
    std::printf(
        "  => aggregate usable capacity %llu covers the required group %llu, yet the largest\n"
        "     single legal placement domain holds only %llu: the group cannot be assembled in\n"
        "     one domain. This is fragmentation, not a shortage.\n",
        static_cast<unsigned long long>(before.value().aggregate_usable),
        static_cast<unsigned long long>(before.value().required_per_group),
        static_cast<unsigned long long>(before.value().largest_legal_group));
  }

  section("topology repair published by the upstream scheduler");
  const fo::Status repaired = scenario.federation.run(fo::SyntheticStep::RepairTopology);
  if (!repaired.ok()) {
    return fail(repaired, "publish the topology repair");
  }
  const fo::Result<fo::FragmentationFinding> after = analyse(scenario, workload_class);
  if (!after.ok()) {
    return fail(after, "fragmentation after the repair");
  }
  std::printf("%s\n", after.value().render("  ").c_str());

  section("recomputed classification");
  {
    std::vector<std::vector<std::string>> rows;
    rows.push_back({"required_per_group", std::to_string(before.value().required_per_group),
                    std::to_string(after.value().required_per_group)});
    rows.push_back({"aggregate_nominal", std::to_string(before.value().aggregate_nominal),
                    std::to_string(after.value().aggregate_nominal)});
    rows.push_back({"aggregate_usable", std::to_string(before.value().aggregate_usable),
                    std::to_string(after.value().aggregate_usable)});
    rows.push_back({"aggregate_stranded", std::to_string(before.value().aggregate_stranded),
                    std::to_string(after.value().aggregate_stranded)});
    rows.push_back({"largest_legal_group", std::to_string(before.value().largest_legal_group),
                    std::to_string(after.value().largest_legal_group)});
    rows.push_back({"largest_domain", fo::render_id(before.value().largest_domain.value()),
                    fo::render_id(after.value().largest_domain.value())});
    rows.push_back({"legal_domain_count", std::to_string(before.value().legal_domain_count),
                    std::to_string(after.value().legal_domain_count)});
    rows.push_back({"classification", std::string(fo::to_string(before.value().classification)),
                    std::string(fo::to_string(after.value().classification))});
    rows.push_back({"digest", before.value().digest(), after.value().digest()});
    std::printf("%s\n", fo::render_table({"field", "before repair", "after repair"}, rows, "  ").c_str());
  }

  if (after.value().largest_legal_group < after.value().required_per_group) {
    std::printf(
        "FAILED [repair]: the repair did not make any single legal domain large enough:\n"
        "  largest_legal_group=%llu required_per_group=%llu\n",
        static_cast<unsigned long long>(after.value().largest_legal_group),
        static_cast<unsigned long long>(after.value().required_per_group));
    return 1;
  }
  std::printf(
      "  The repaired topology places a full group in one legal domain, so the recomputed\n"
      "  classification is %s. The earlier finding is superseded by the new topology\n"
      "  generation; both are printed above rather than the earlier one being amended.\n",
      std::string(fo::to_string(after.value().classification)).c_str());
  std::printf(
      "  Federation Observatory grouped observed capacity into observed domains. It did not\n"
      "  place the group, did not repair the federation, and does not own placement policy.\n");
  if (!premise_holds) {
    std::printf(
        "FAILED [premise]: the published scenario does not satisfy the property this example\n"
        "  demonstrates. It needs (a) usable capacity for wc-train that sums to at least %llu\n"
        "  across the federation, and (b) no single legal placement domain holding %llu. Today\n"
        "  aggregate_usable=%llu with largest_legal_group=%llu.\n",
        static_cast<unsigned long long>(before.value().required_per_group),
        static_cast<unsigned long long>(before.value().required_per_group),
        static_cast<unsigned long long>(before.value().aggregate_usable),
        static_cast<unsigned long long>(before.value().largest_legal_group));
    return 1;
  }
  return 0;
}
