// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// The synthetic federation backend driven through the production in-process pipeline.

#include <string>

#include "federation_observatory/synthetic.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

struct Scenario {
  fo::FederationObservatory observatory;
  fo::PublisherId publisher{"synth-publisher"};
  fo::BootGeneration boot{1};
  std::unique_ptr<fo::ObservatorySink> sink;
  std::unique_ptr<fo::SyntheticFederation> scenario;

  Scenario() {
    sink = std::make_unique<fo::ObservatorySink>(observatory, publisher, boot,
                                                 fo::FederationId::unchecked("synth-fed"));
    fo::SyntheticConfig config;
    config.federation = fo::FederationId::unchecked("synth-fed");
    config.publisher = publisher;
    config.boot = boot;
    scenario = std::make_unique<fo::SyntheticFederation>(config, *sink);
  }

  [[nodiscard]] fo::Status run_all() { return scenario->run_all(); }
};

}  // namespace

FO_TEST(synthetic, full_scenario_publishes_every_record_kind) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
  FO_CHECK_EQ(snapshot->sites.size(), 3u);
  FO_CHECK_EQ(snapshot->clusters.size(), 6u);
  FO_CHECK_EQ(snapshot->accelerator_classes.size(), 3u);
  FO_CHECK_EQ(snapshot->workloads.size(), 3u);
  FO_CHECK_EQ(snapshot->placements.size(), 3u);
  FO_CHECK_EQ(snapshot->migrations.size(), 2u);
  FO_CHECK_EQ(snapshot->portability.size(), 2u);
  FO_CHECK(snapshot->find_federation(fo::FederationId::unchecked("synth-fed")) != nullptr);
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    FO_CHECK(cluster.currentness == fo::Currentness::Current);
  }
}

FO_TEST(synthetic, scenario_is_deterministic_across_runs) {
  Scenario first;
  Scenario second;
  FO_REQUIRE(first.run_all().ok());
  FO_REQUIRE(second.run_all().ok());
  FO_CHECK_EQ(first.observatory.snapshot()->digest(), second.observatory.snapshot()->digest());
}

FO_TEST(synthetic, placements_carry_rejections_and_expose_their_coverage) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
  const fo::PlacementRecord* partial =
      snapshot->find_placement(fo::PlacementId::unchecked("placement-1"));
  FO_REQUIRE(partial != nullptr);
  FO_CHECK(partial->candidate_completeness == fo::CandidateSetCompleteness::Partial);
  FO_CHECK_EQ(partial->rejected_count(), 2u);
  FO_CHECK_EQ(partial->find_candidate(fo::ClusterId::unchecked("cluster-1-0"))->reason_list().size(),
              3u);

  const fo::PlacementRecord* selected_only =
      snapshot->find_placement(fo::PlacementId::unchecked("placement-2"));
  FO_REQUIRE(selected_only != nullptr);
  FO_CHECK(selected_only->candidate_completeness == fo::CandidateSetCompleteness::SelectedOnly);
  const fo::Result<fo::PlacementExplanation> explanation =
      scenario.observatory.explain_placement(selected_only->id);
  FO_REQUIRE(explanation.ok());
  FO_CHECK(!explanation.value().has_rejection_attribution());
}

FO_TEST(synthetic, stranded_capacity_is_computed_over_the_synthetic_fleet) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  fo::StrandedCapacityRequest request;
  request.federation = fo::FederationId::unchecked("synth-fed");
  request.workload_class = fo::WorkloadClassId::unchecked("wc-train");
  const fo::StrandedCapacityReport report =
      FO_UNWRAP(scenario.observatory.stranded_capacity(request));
  FO_CHECK(report.closes());
  FO_CHECK(report.summary.nominal > 0);
  FO_CHECK(report.summary.stranded > 0);
  FO_CHECK(report.summary.usable > 0);
  FO_CHECK_EQ(report.summary.usable + report.summary.stranded + report.summary.unknown,
              report.summary.idle);
  FO_CHECK(report.digest() == FO_UNWRAP(scenario.observatory.stranded_capacity(request)).digest());
}

FO_TEST(synthetic, fragmentation_is_detected_for_the_wide_workload) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::FragmentationFinding finding =
      FO_UNWRAP(scenario.observatory.fragmentation(fo::FederationId::unchecked("synth-fed"),
                                                   fo::WorkloadClassId::unchecked("wc-train"),
                                                   fo::ResourceKind::Accelerator));
  FO_CHECK_EQ(finding.required_per_group, 8u);
  FO_CHECK(finding.aggregate_nominal >= 8);
  FO_CHECK(finding.classification != fo::FragmentationClass::None);
}

FO_TEST(synthetic, repair_topology_changes_the_fragmentation_verdict) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::FragmentationFinding before =
      FO_UNWRAP(scenario.observatory.fragmentation(fo::FederationId::unchecked("synth-fed"),
                                                   fo::WorkloadClassId::unchecked("wc-train"),
                                                   fo::ResourceKind::Accelerator));
  FO_REQUIRE(scenario.scenario->run(fo::SyntheticStep::RepairTopology).ok());
  const fo::FragmentationFinding after =
      FO_UNWRAP(scenario.observatory.fragmentation(fo::FederationId::unchecked("synth-fed"),
                                                   fo::WorkloadClassId::unchecked("wc-train"),
                                                   fo::ResourceKind::Accelerator));
  FO_CHECK(after.largest_legal_group >= before.largest_legal_group);
  FO_CHECK(after.digest() != before.digest());
  FO_CHECK(after.classification != before.classification ||
           after.largest_legal_group > before.largest_legal_group);
}

FO_TEST(synthetic, capability_change_invalidates_the_previous_capability_state) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::SnapshotHandle before = scenario.observatory.snapshot();
  FO_CHECK_EQ(before->find_cluster(fo::ClusterId::unchecked("cluster-0-0"))
                  ->capability_generation.value(),
              1u);
  FO_REQUIRE(scenario.scenario->run(fo::SyntheticStep::CapabilityChange).ok());
  const fo::SnapshotHandle after = scenario.observatory.snapshot();
  FO_CHECK_EQ(after->find_cluster(fo::ClusterId::unchecked("cluster-0-0"))
                  ->capability_generation.value(),
              2u);
  fo::CapabilityRef fp8;
  fp8.key = fo::CapabilityKey::PrecisionFp8E4M3;
  FO_CHECK(after->find_cluster(fo::ClusterId::unchecked("cluster-0-0"))->capabilities.contains(fp8) ==
           false);
}

FO_TEST(synthetic, cluster_join_and_leave_change_the_membership) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const std::size_t before = scenario.observatory.snapshot()->clusters.size();
  FO_REQUIRE(scenario.scenario->run(fo::SyntheticStep::ClusterJoin).ok());
  FO_CHECK_EQ(scenario.observatory.snapshot()->clusters.size(), before + 1);
  FO_REQUIRE(scenario.scenario->run(fo::SyntheticStep::ClusterLeave).ok());
  const fo::SnapshotHandle after = scenario.observatory.snapshot();
  FO_CHECK_EQ(after->health.clusters_retired, 1u);
  FO_CHECK(after->health.degraded);
}

FO_TEST(synthetic, stale_evidence_advances_a_cluster_generation_and_drops_its_capacity) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  FO_REQUIRE(scenario.scenario->run(fo::SyntheticStep::StaleEvidence).ok());
  const fo::SnapshotHandle snapshot = scenario.observatory.snapshot();
  bool found = false;
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    if (cluster.generation.value() == 2) {
      found = true;
      FO_CHECK(cluster.epoch.value() == 2);
      FO_CHECK(cluster.capacity_pools.empty());
    }
  }
  FO_CHECK(found);
}

FO_TEST(synthetic, migration_analysis_covers_supersession_and_adaptation) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::Result<fo::MigrationAnalysis> first =
      scenario.observatory.migration_analysis(fo::MigrationId::unchecked("migration-1"));
  FO_REQUIRE(first.ok());
  FO_CHECK(first.value().outcome == fo::MigrationOutcome::Committed);
  FO_CHECK(first.value().superseded);
  FO_CHECK_EQ(first.value().superseded_by.value(), std::string("migration-2"));
  FO_CHECK(first.value().requires_adaptation);

  const fo::Result<fo::MigrationAnalysis> second =
      scenario.observatory.migration_analysis(fo::MigrationId::unchecked("migration-2"));
  FO_REQUIRE(second.ok());
  FO_CHECK(second.value().outcome == fo::MigrationOutcome::InProgress);
  FO_CHECK(!second.value().superseded);
}

FO_TEST(synthetic, late_events_from_a_superseded_migration_are_refused) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const fo::Result<fo::IngestResult> late = scenario.observatory.publish_migration_stage(
      scenario.sink->context(fo::EvidenceGeneration{1}),
      fo::MigrationId::unchecked("migration-1"), fo::MigrationGeneration{1},
      fo::MigrationStage::Committed, "late replay");
  FO_CHECK(!late.ok());
  // The migration was superseded, so its generation is historical: the refusal names
  // staleness rather than an illegal transition.
  FO_CHECK(late.error().code() == fo::ErrorCode::StaleGeneration);
}

FO_TEST(synthetic, mismatch_analysis_reports_populations_and_denominators) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  fo::MismatchAnalysisRequest request;
  request.federation = fo::FederationId::unchecked("synth-fed");
  const fo::MismatchAnalysis analysis = FO_UNWRAP(scenario.observatory.mismatch_analysis(request));
  FO_CHECK(!analysis.by_capability_key.empty());
  FO_CHECK_EQ(analysis.placements_observed, 3u);
  FO_CHECK_EQ(analysis.migrations_observed, 2u);
  FO_CHECK_EQ(analysis.portability_records_observed, 2u);
  bool found_fp8 = false;
  for (const fo::MismatchRow& row : analysis.by_capability_key) {
    if (row.subject == "precision.fp8_e4m3") {
      found_fp8 = true;
      FO_CHECK_EQ(row.population, 6u);
      FO_CHECK(row.missing > 0);
    }
  }
  FO_CHECK(found_fp8);
  FO_CHECK(analysis.render().find("not about the hardware") != std::string::npos);
}

FO_TEST(synthetic, per_step_execution_matches_the_full_run) {
  Scenario stepwise;
  fo::SyntheticConfig config;
  config.federation = fo::FederationId::unchecked("synth-fed");
  fo::ObservatorySink sink(stepwise.observatory, fo::PublisherId::unchecked("synth-publisher"),
                           fo::BootGeneration{1}, fo::FederationId::unchecked("synth-fed"));
  fo::SyntheticFederation scenario(config, sink);
  FO_REQUIRE(sink.register_self().ok());
  const fo::SyntheticStep order[] = {
      fo::SyntheticStep::AcceleratorClasses, fo::SyntheticStep::Runtimes,
      fo::SyntheticStep::Backends,           fo::SyntheticStep::Topology,
      fo::SyntheticStep::Policies,           fo::SyntheticStep::Domains,
      fo::SyntheticStep::Artifacts,          fo::SyntheticStep::WorkloadClasses,
      fo::SyntheticStep::Workloads,          fo::SyntheticStep::Capabilities,
      fo::SyntheticStep::Capacity,           fo::SyntheticStep::Placements,
      fo::SyntheticStep::Migrations,         fo::SyntheticStep::Portability,
  };
  for (const fo::SyntheticStep step : order) {
    FO_REQUIRE(scenario.run(step).ok());
  }
  Scenario full;
  FO_REQUIRE(full.run_all().ok());
  FO_CHECK_EQ(stepwise.observatory.snapshot()->digest(), full.observatory.snapshot()->digest());
}

FO_TEST(synthetic, reproduction_record_is_populated) {
  Scenario scenario;
  FO_REQUIRE(scenario.run_all().ok());
  const std::string record = scenario.scenario->reproduction_record();
  FO_CHECK(record.find("seed") != std::string::npos);
  FO_CHECK(record.find("synth-fed") != std::string::npos);
  FO_CHECK(!scenario.scenario->last_error().empty() == false);
}
