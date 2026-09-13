// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_portability_failure - a technical failure and a policy block are not the same thing.
//
// What this proves:
//   * all eleven portability dimensions are reported for a destination, each with its own
//     outcome, basis, precision and detail - portability is never one boolean;
//   * a destination that fails on technical grounds and a destination that is refused by
//     policy produce different findings: technically_blocked and policy_blocked are
//     carried separately and neither is folded into the other;
//   * a dimension with no evidence is reported as UNKNOWN, never as PORTABLE_DIRECT.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic) and is
// NOT a physical federation. The policy used here is a synthetic data-residency policy;
// Federation Observatory evaluates admissibility from published policy evidence and does
// not own, grant or waive any policy.

#include <cstdio>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/portability.hpp"
#include "federation_observatory/version.hpp"
#include "federation_observatory/synthetic.hpp"

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

int print_case(const Scenario& scenario, const char* title, const fo::WorkloadId& workload,
               const fo::ClusterId& destination) {
  const fo::Result<fo::PortabilityAssessment> assessment =
      scenario.observatory.evaluate_portability(workload, destination);
  if (!assessment.ok()) {
    return fail(assessment, title);
  }
  const fo::PortabilityAssessment& value = assessment.value();
  std::printf("  workload %s -> destination %s\n", workload.value().c_str(),
              destination.value().c_str());
  std::printf("  overall                      %s\n",
              std::string(fo::to_string(value.overall)).c_str());
  std::printf("  dimensions_reported          %zu\n", value.dimensions.size());
  std::printf("  technically_blocked          %s\n", value.technically_blocked ? "yes" : "no");
  std::printf("  policy_blocked               %s\n", value.policy_blocked ? "yes" : "no");
  std::printf("  requires_rebuild/recompile   %s/%s\n", value.requires_rebuild ? "yes" : "no",
              value.requires_recompile ? "yes" : "no");
  std::printf("  requires_conversion/state    %s/%s\n", value.requires_conversion ? "yes" : "no",
              value.requires_state_translation ? "yes" : "no");
  std::printf("  currentness / precision      %s / %s\n",
              std::string(fo::to_string(value.currentness)).c_str(),
              std::string(fo::to_string(value.precision)).c_str());

  std::vector<std::vector<std::string>> rows;
  for (const fo::PortabilityDimensionResult& result : value.dimensions) {
    rows.push_back({std::string(fo::to_string(result.dimension)),
                    std::string(fo::to_string(result.outcome)),
                    std::string(fo::to_string(result.basis)),
                    std::string(fo::to_string(result.precision)),
                    fo::is_technical_failure(result.outcome)
                        ? "technical failure"
                        : (fo::is_policy_failure(result.outcome)
                               ? "policy restriction"
                               : (fo::is_undecided(result.outcome) ? "undecided" : "no failure")),
                    result.detail});
  }
  std::printf("%s\n", fo::render_table({"portability_dimension", "outcome", "basis", "precision",
                                      "reading", "detail"},
                                     rows, "  ")
                         .c_str());
  return 0;
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: portability failure\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example evaluates two SYNTHETIC migration destinations: one that fails for\n"
      "technical reasons and one that is blocked by policy, printing all eleven dimensions\n"
      "of each so that the two are visibly not conflated.\n");

  Scenario scenario;
  const fo::Status registered = scenario.sink.register_self();
  if (!registered.ok()) {
    return fail(registered, "register publisher");
  }
  const fo::Status published = scenario.federation.run_all();
  if (!published.ok()) {
    return fail(published, "publish synthetic federation");
  }

  const fo::WorkloadId infer = fo::WorkloadId::unchecked("wl-infer-1");
  const fo::WorkloadId legacy = fo::WorkloadId::unchecked("wl-legacy-1");

  section("case A: destination refused on technical grounds (wl-infer-1 -> cluster-1-0)");
  const int technical = print_case(scenario, "case A", infer, fo::ClusterId::unchecked("cluster-1-0"));
  if (technical != 0) {
    return technical;
  }

  section("case B: destination refused by policy (wl-legacy-1 -> cluster-2-1)");
  const int policy = print_case(scenario, "case B", legacy, fo::ClusterId::unchecked("cluster-2-1"));
  if (policy != 0) {
    return policy;
  }

  section("the two findings are not conflated");
  {
    const fo::Result<fo::PortabilityAssessment> first =
        scenario.observatory.evaluate_portability(infer, fo::ClusterId::unchecked("cluster-1-0"));
    const fo::Result<fo::PortabilityAssessment> second =
        scenario.observatory.evaluate_portability(legacy, fo::ClusterId::unchecked("cluster-2-1"));
    if (!first.ok() || !second.ok()) {
      std::printf("FAILED [conflation]: an assessment became unavailable on re-evaluation\n");
      return 1;
    }
    std::vector<std::vector<std::string>> rows;
    rows.push_back({"technically_blocked", first.value().technically_blocked ? "yes" : "no",
                    second.value().technically_blocked ? "yes" : "no"});
    rows.push_back({"policy_blocked", first.value().policy_blocked ? "yes" : "no",
                    second.value().policy_blocked ? "yes" : "no"});
    rows.push_back({"overall", std::string(fo::to_string(first.value().overall)),
                    std::string(fo::to_string(second.value().overall))});
    std::printf("%s\n", fo::render_table({"flag", "case A (technical)", "case B (policy)"}, rows, "  ")
                           .c_str());

    if (!first.value().technically_blocked || first.value().policy_blocked) {
      std::printf("FAILED [case A]: this destination was expected to fail technically only\n");
      return 1;
    }
    if (!second.value().policy_blocked || second.value().technically_blocked) {
      std::printf("FAILED [case B]: this destination was expected to be blocked by policy only\n");
      return 1;
    }
    std::printf(
        "  Case A fails because the destination cannot run the workload generation: the\n"
        "  evidence names the incompatible dimension. Case B fails because policy generation\n"
        "  excludes the site: the destination may be perfectly capable and is still refused.\n"
        "  is_technical_failure() is true only for %s; is_policy_failure() is true only for %s.\n",
        std::string(fo::to_string(fo::PortabilityOutcome::NotPortableArchitecture)).c_str(),
        std::string(fo::to_string(fo::PortabilityOutcome::NotPortablePolicy)).c_str());
    std::printf(
        "  A rebuild, a recompile or a conversion can fix case A. No technical remedy exists\n"
        "  for case B: the remedy belongs to the policy owner, which is not this runtime.\n");
  }
  return 0;
}
