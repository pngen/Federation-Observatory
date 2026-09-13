// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// ex_migration_analysis - what an observed migration does and does not prove.
//
// What this proves:
//   * a migration is reported as an ordered set of observed stages, bound to the
//     migration generation that produced them, with its source/destination pair;
//   * portability is reported across all eleven independent dimensions and is never
//     collapsed into one boolean;
//   * a migration that was superseded is reported as superseded, its record stops being
//     current, and a late event bearing the superseded generation is refused;
//   * a COMMITTED migration does not prove byte-for-byte state portability. The runtime
//     asserts state portability only from a directly portable State dimension carrying
//     exact evidence, and never asserts performance portability at all.
//
// The scenario is SYNTHETIC (fo::SyntheticFederation, EvidenceClass::Synthetic) and is
// NOT a physical federation. The migration is an observation of what the upstream
// migration runtime did; Federation Observatory never migrates anything itself.

#include <cstdio>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/portability.hpp"
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

void print_stages(const fo::MigrationRecord& migration) {
  std::vector<std::vector<std::string>> rows;
  for (const fo::MigrationStageEvent& event : migration.stage_events) {
    rows.push_back({event.generation.is_set() ? event.generation.to_string() : std::string("-"),
                    std::string(fo::to_string(event.stage)), event.sequence.to_string(),
                    event.detail.empty() ? "-" : event.detail});
  }
  std::printf("%s\n", fo::render_table({"generation", "stage", "sequence", "detail"}, rows, "  ").c_str());
}

void print_dimensions(const fo::PortabilityAssessment& portability) {
  std::vector<std::vector<std::string>> rows;
  for (const fo::PortabilityDimensionResult& result : portability.dimensions) {
    rows.push_back({std::string(fo::to_string(result.dimension)),
                    std::string(fo::to_string(result.outcome)),
                    std::string(fo::to_string(result.basis)),
                    std::string(fo::to_string(result.precision)), result.detail});
  }
  std::printf("%s\n", fo::render_table({"portability_dimension", "outcome", "basis", "precision",
                                      "detail"},
                                     rows, "  ")
                         .c_str());
}

}  // namespace

int main() {
  std::printf("Federation Observatory %s - example: migration analysis\n",
              std::string(fo::version_string()).c_str());
  std::printf(
      "This example analyses a SYNTHETIC migration across all portability dimensions, then\n"
      "shows the superseded case and states exactly what a committed migration proves.\n");

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

  const fo::MigrationId first_id = fo::MigrationId::unchecked("migration-1");
  const fo::MigrationId second_id = fo::MigrationId::unchecked("migration-2");
  const fo::MigrationRecord* first = snapshot->find_migration(first_id);
  const fo::MigrationRecord* second = snapshot->find_migration(second_id);
  if (first == nullptr || second == nullptr) {
    std::printf("FAILED [migrations]: the synthetic scenario published no migration pair\n");
    return 1;
  }

  section("observed migration stages");
  std::printf("  migration %s generation %s: %s -> %s, stage %s, outcome %s\n",
              first->id.value().c_str(), first->generation.to_string().c_str(),
              first->source.value().c_str(), first->destination.value().c_str(),
              std::string(fo::to_string(first->stage)).c_str(),
              std::string(fo::to_string(first->outcome)).c_str());
  print_stages(*first);

  section("migration analysis");
  const fo::Result<fo::MigrationAnalysis> analysis =
      scenario.observatory.migration_analysis(first_id);
  if (!analysis.ok()) {
    return fail(analysis, "migration analysis");
  }
  std::printf("%s\n", analysis.value().render().c_str());

  section("portability dimensions of the analysed pair");
  print_dimensions(analysis.value().portability);
  std::printf("  dimensions reported: %zu (all independent; portability is never one boolean)\n",
              analysis.value().portability.dimensions.size());

  section("the superseded migration");
  std::printf("  %s supersedes %s (generation %s supersedes generation %s)\n",
              second->id.value().c_str(), fo::render_id(second->supersedes.value()).c_str(),
              second->generation.to_string().c_str(), second->supersedes_generation.to_string().c_str());
  std::printf("  migration-1 currentness: %s\n",
              std::string(fo::to_string(first->currentness)).c_str());
  std::printf("  analysis.superseded: %s, superseded_by: %s\n",
              analysis.value().superseded ? "yes" : "no",
              fo::render_id(analysis.value().superseded_by.value()).c_str());
  std::printf("  late event from the superseded generation is stale: %s\n",
              fo::is_stale_migration_event(*second, fo::MigrationGeneration{1}, fo::Sequence{1})
                  ? "yes"
                  : "no");
  {
    const fo::Status late = scenario.sink.emit_migration_stage(
        second_id, fo::MigrationGeneration{1}, fo::MigrationStage::SourceQuiescing,
        "late event replayed from the superseded generation");
    std::printf("  publication of that late event: %s\n", late.to_string().c_str());
    if (late.ok()) {
      std::printf("FAILED [supersession]: a late event from the superseded generation was accepted\n");
      return 1;
    }
  }

  section("what a committed migration proves");
  std::printf("  migration-1 stage/outcome: %s / %s\n",
              std::string(fo::to_string(first->stage)).c_str(),
              std::string(fo::to_string(first->outcome)).c_str());
  std::printf("  state_portability_proven:   %s\n",
              std::string(fo::to_string(analysis.value().state_portability_proven)).c_str());
  std::printf("  performance_portability_proven: %s\n",
              std::string(fo::to_string(analysis.value().performance_portability_proven)).c_str());
  std::printf(
      "  A committed migration proves that the workload reached a terminal COMMITTED stage at\n"
      "  the destination. It does NOT prove byte-for-byte state portability: state is asserted\n"
      "  portable only when the State dimension itself is directly portable under exact\n"
      "  evidence, which is reported above as %s. Performance portability is never asserted\n"
      "  because this runtime does not measure workload performance.\n",
      std::string(fo::to_string(analysis.value().state_portability_proven)).c_str());
  std::printf(
      "  Federation Observatory observed this migration; it did not perform it, and it does\n"
      "  not own migration or rollback policy.\n");
  return 0;
}
