// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_placement_explanation - explain one observed placement decision.
//
// What this proves:
//   * an explanation separates the observed selection from the constraints the source
//     stated, and cites the compatibility checks that make the chosen cluster eligible;
//   * every alternative the scheduler exposed is listed with the structured rejection
//     reasons it carried, keeping distinct causes distinct;
//   * when the upstream scheduler exposed only the selected target, the runtime reports
//     "rejection attribution unavailable" instead of inventing a reason.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic) and is
// NOT a physical federation. The placement records are observations of what an upstream
// scheduler published; Federation Observatory neither chose the target nor owns policy.

#include <cstdio>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/placement.hpp"
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

std::string reason_names(const fo::CandidateObservation& candidate) {
  std::string names;
  for (const fo::RejectionReason reason : candidate.reason_list()) {
    if (!names.empty()) {
      names += ",";
    }
    names += std::string(fo::to_string(reason));
  }
  return names.empty() ? std::string("-") : names;
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: placement explanation\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example explains a SYNTHETIC placement that the upstream scheduler published.\n"
      "It attributes the selection and every exposed rejection to published evidence, and\n"
      "states where attribution is unavailable because only the selected target was exposed.\n");

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

  section("placements published by the upstream scheduler");
  {
    std::vector<std::vector<std::string>> rows;
    for (const fo::PlacementRecord& placement : snapshot->placements) {
      rows.push_back({placement.id.value(), placement.workload.value(),
                      placement.workload_class.value(), placement.selected.value(),
                      std::string(fo::to_string(placement.candidate_completeness)),
                      std::to_string(placement.candidates.size()),
                      std::to_string(placement.rejected_count()),
                      std::string(fo::to_string(placement.currentness))});
    }
    std::printf("%s\n", fo::render_table({"placement", "workload", "workload_class", "selected",
                                        "candidate_set", "candidates", "rejected", "currentness"},
                                       rows, "  ")
                           .c_str());
  }

  section("placement-1: selected target, exposed alternatives, stated constraints");
  {
    const fo::Result<fo::PlacementExplanation> explanation =
        scenario.observatory.explain_placement(fo::PlacementId::unchecked("placement-1"));
    if (!explanation.ok()) {
      return fail(explanation, "explain placement-1");
    }
    std::printf("%s\n", explanation.value().render().c_str());
  }

  section("placement-1: why each exposed alternative was rejected");
  {
    const fo::PlacementRecord* record =
        snapshot->find_placement(fo::PlacementId::unchecked("placement-1"));
    if (record == nullptr) {
      std::printf("FAILED [placement-1]: the snapshot does not hold this placement\n");
      return 1;
    }
    for (const fo::CandidateObservation& candidate : record->candidates) {
      if (candidate.status == fo::CandidateStatus::Selected) {
        continue;
      }
      std::printf("  candidate %s: status=%s reasons=%s\n", candidate.cluster.value().c_str(),
                  std::string(fo::to_string(candidate.status)).c_str(),
                  reason_names(candidate).c_str());
      const fo::Result<fo::RejectionExplanation> rejection =
          scenario.observatory.explain_rejection(record->id, candidate.cluster);
      if (!rejection.ok()) {
        return fail(rejection, "explain rejection");
      }
      std::printf("%s\n", rejection.value().render().c_str());
    }
  }

  section("placement-2: the scheduler exposed only the selected target");
  {
    const fo::Result<fo::PlacementExplanation> explanation =
        scenario.observatory.explain_placement(fo::PlacementId::unchecked("placement-2"));
    if (!explanation.ok()) {
      return fail(explanation, "explain placement-2");
    }
    std::printf("%s\n", explanation.value().render().c_str());
    std::printf("  rejection_attribution_available: %s\n",
                explanation.value().has_rejection_attribution() ? "yes" : "no");

    const fo::Result<fo::RejectionExplanation> unavailable = scenario.observatory.explain_rejection(
        fo::PlacementId::unchecked("placement-2"), fo::ClusterId::unchecked("cluster-1-0"));
    if (!unavailable.ok()) {
      return fail(unavailable, "explain rejection on a selected-only candidate set");
    }
    std::printf("%s\n", unavailable.value().render().c_str());

    const fo::Result<fo::RejectionExplanation> absent = scenario.observatory.explain_rejection(
        fo::PlacementId::unchecked("placement-2"), fo::ClusterId::unchecked("cluster-9-9"));
    if (!absent.ok()) {
      return fail(absent, "explain rejection of a cluster that was never exposed");
    }
    std::printf("%s\n", absent.value().render().c_str());
  }

  section("attribution");
  std::printf(
      "  The selection is an OBSERVED fact reported by the scheduler; the eligibility and\n"
      "  compatibility findings are DERIVED by this runtime from published evidence, and the\n"
      "  rejections are what the scheduler stated. Where the candidate set was not exposed,\n"
      "  the runtime says so: absent evidence is reported as unknown, never as a rejection.\n"
      "  Federation Observatory did not choose this placement and does not own placement policy.\n");
  return 0;
}
