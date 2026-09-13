// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_capability_mismatch - aggregate capability mismatch over a published federation.
//
// What this proves:
//   * fo::analyze_mismatch reports, per capability key and per subject, an explicit
//     population together with satisfied / missing / unknown counts, so no percentage
//     can be read without its denominator;
//   * "missing" is a statement about the PUBLISHED EVIDENCE - the publisher did not
//     declare the capability - and never a claim about the hardware;
//   * "unknown" means the published evidence does not decide the question, and is kept
//     separate from both satisfied and missing.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic): the
// mismatch is manufactured by the deterministic generator and is NOT a statement about
// any physical fleet or about any vendor's hardware.

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

void print_rows(const std::vector<fo::MismatchRow>& rows) {
  if (rows.empty()) {
    std::printf("  <none>\n");
    return;
  }
  std::vector<std::vector<std::string>> table;
  for (const fo::MismatchRow& row : rows) {
    table.push_back({row.subject, std::to_string(row.population), std::to_string(row.satisfied),
                     std::to_string(row.missing), std::to_string(row.unknown),
                     std::string(fo::to_string(row.precision))});
  }
  std::printf("%s\n", fo::render_table({"subject", "population", "satisfied", "missing", "unknown",
                                      "precision"},
                                     table, "  ")
                         .c_str());
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: capability mismatch\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example runs the aggregate mismatch analysis over a SYNTHETIC federation and\n"
      "prints population / satisfied / missing / unknown for every capability subject.\n");

  Scenario scenario;
  const fo::Status registered = scenario.sink.register_self();
  if (!registered.ok()) {
    return fail(registered, "register publisher");
  }
  const fo::Status published = scenario.federation.run_all();
  if (!published.ok()) {
    return fail(published, "publish synthetic federation");
  }

  fo::MismatchAnalysisRequest request;
  request.federation = scenario.config.federation;
  request.kind = fo::ResourceKind::Accelerator;
  request.include_stale = false;

  const fo::Result<fo::MismatchAnalysis> analysis = scenario.observatory.mismatch_analysis(request);
  if (!analysis.ok()) {
    return fail(analysis, "mismatch analysis");
  }
  const fo::MismatchAnalysis& report = analysis.value();

  section("report");
  std::printf("  federation                   %s\n", fo::render_id(report.federation.value()).c_str());
  std::printf("  snapshot_generation          %s\n",
              report.snapshot_generation.is_set() ? report.snapshot_generation.to_string().c_str()
                                                  : "-");
  std::printf("  window                       %s\n", report.window.render().c_str());
  std::printf("  placements_observed          %llu\n",
              static_cast<unsigned long long>(report.placements_observed));
  std::printf("  migrations_observed          %llu\n",
              static_cast<unsigned long long>(report.migrations_observed));
  std::printf("  portability_records_observed %llu\n",
              static_cast<unsigned long long>(report.portability_records_observed));
  std::printf("  precision                    %s\n",
              std::string(fo::to_string(report.precision)).c_str());
  std::printf("  evidence_class               %s\n",
              std::string(fo::to_string(report.evidence_class)).c_str());
  std::printf("  digest                       %s\n", report.digest().c_str());

  section("by capability key");
  print_rows(report.by_capability_key);

  section("by cluster");
  print_rows(report.by_cluster);

  section("what the columns mean");
  std::printf(
      "  population : the number of subjects this row was computed over. Every count below is\n"
      "               read against this denominator; the analysis never prints a bare percentage.\n"
      "  satisfied  : the published evidence declares the capability and the requirement holds.\n"
      "  missing    : the publisher did NOT DECLARE the capability.  <-- this is the explicit\n"
      "               meaning of \"missing\": it is a statement about the published evidence,\n"
      "               not about the hardware, and not proof that the hardware lacks it.\n"
      "  unknown    : the published evidence does not decide the question.\n");
  std::printf("  by_capability_key rows: %zu\n", report.by_capability_key.size());

  section("one capability key in isolation");
  {
    fo::MismatchAnalysisRequest filtered;
    filtered.federation = scenario.config.federation;
    filtered.capability_key.key = fo::CapabilityKey::PrecisionFp8E4M3;
    filtered.kind = fo::ResourceKind::Accelerator;
    const fo::Result<fo::MismatchAnalysis> narrow =
        scenario.observatory.mismatch_analysis(filtered);
    if (!narrow.ok()) {
      return fail(narrow, "mismatch analysis for one capability key");
    }
    std::printf("  requested capability         %s\n",
                std::string(fo::to_string(fo::CapabilityKey::PrecisionFp8E4M3)).c_str());
    print_rows(narrow.value().by_capability_key);
    std::printf("  Where a cluster does not declare %s, the row above says so; the runtime does\n",
                std::string(fo::to_string(fo::CapabilityKey::PrecisionFp8E4M3)).c_str());
    std::printf("  not conclude that the accelerator is incapable of it.\n");
  }

  section("full report as rendered by the runtime");
  std::printf("%s\n", report.render().c_str());
  return 0;
}
