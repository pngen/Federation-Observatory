// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Migration observation: stage machine, generation fencing, supersession, and the
// explicit refusal to infer causality or state portability that was not observed.

#include "federation_observatory/migration.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

MigrationRecord make_migration() {
  MigrationRecord migration;
  migration.id = MigrationId::unchecked("migration-1");
  migration.generation = MigrationGeneration{1};
  migration.workload = WorkloadId::unchecked("wl-1");
  migration.workload_generation = WorkloadGeneration{1};
  migration.federation = FederationId::unchecked("fed-1");
  migration.federation_generation = FederationGeneration{1};
  migration.source = ClusterId::unchecked("cluster-a");
  migration.source_generation = ClusterGeneration{1};
  migration.source_epoch = ClusterEpoch{1};
  migration.destination = ClusterId::unchecked("cluster-b");
  migration.destination_generation = ClusterGeneration{1};
  migration.destination_epoch = ClusterEpoch{1};
  migration.source_runtime = RuntimeId::unchecked("runtime-cuda");
  migration.destination_runtime = RuntimeId::unchecked("runtime-rocm");
  migration.stamp.precision = Precision::Exact;
  migration.stamp.evidence_class = EvidenceClass::Synthetic;
  migration.currentness = Currentness::Current;
  MigrationStageEvent planned;
  planned.generation = MigrationGeneration{1};
  planned.stage = MigrationStage::Planned;
  planned.sequence = Sequence{1};
  migration.stage_events.push_back(planned);
  return migration;
}

MigrationStageEvent stage_event(MigrationStage stage, std::uint64_t sequence) {
  MigrationStageEvent event;
  event.generation = MigrationGeneration{1};
  event.stage = stage;
  event.sequence = Sequence(sequence);
  return event;
}

}  // namespace

FO_TEST(migration, stage_machine_rejects_illegal_transitions) {
  FO_CHECK(is_valid_migration_transition(MigrationStage::Planned, MigrationStage::SourceQuiescing));
  FO_CHECK(is_valid_migration_transition(MigrationStage::Planned, MigrationStage::Failed));
  FO_CHECK(!is_valid_migration_transition(MigrationStage::Planned, MigrationStage::Committed));
  FO_CHECK(!is_valid_migration_transition(MigrationStage::Planned, MigrationStage::RestoreStarted));
  FO_CHECK(is_valid_migration_transition(MigrationStage::RestoreComplete, MigrationStage::Committed));
  FO_CHECK(
      is_valid_migration_transition(MigrationStage::RevalidationRequired, MigrationStage::Committed));
  FO_CHECK(!is_valid_migration_transition(MigrationStage::Committed, MigrationStage::Failed));
  FO_CHECK(!is_valid_migration_transition(MigrationStage::RolledBack, MigrationStage::Committed));
  FO_CHECK(is_valid_migration_transition(MigrationStage::OutcomeUnknown, MigrationStage::Failed));
  FO_CHECK(is_acceptable_migration_event(MigrationStage::Planned, MigrationStage::Planned));
  FO_CHECK(is_terminal_stage(MigrationStage::Committed));
  FO_CHECK(is_terminal_stage(MigrationStage::Failed));
  FO_CHECK(!is_terminal_stage(MigrationStage::RestoreComplete));
}

FO_TEST(migration, record_validation_enforces_the_lifecycle) {
  MigrationRecord migration = make_migration();
  FO_CHECK(migration.validate().ok());

  MigrationRecord mismatched_outcome = make_migration();
  mismatched_outcome.stage = MigrationStage::Committed;
  mismatched_outcome.outcome = MigrationOutcome::InProgress;
  FO_CHECK(!mismatched_outcome.validate().ok());
  FO_CHECK(mismatched_outcome.validate().code() == ErrorCode::InvalidTransition);

  MigrationRecord same_cluster = make_migration();
  same_cluster.destination = same_cluster.source;
  FO_CHECK(!same_cluster.validate().ok());

  MigrationRecord regressed = make_migration();
  regressed.stage_events.push_back(stage_event(MigrationStage::SourceQuiescing, 5));
  regressed.stage_events.push_back(stage_event(MigrationStage::StateCaptured, 4));
  regressed.stage = MigrationStage::StateCaptured;
  FO_CHECK(!regressed.validate().ok());
  FO_CHECK(regressed.validate().code() == ErrorCode::SequenceRegression);

  MigrationRecord illegal = make_migration();
  illegal.stage_events.push_back(stage_event(MigrationStage::Committed, 2));
  illegal.stage = MigrationStage::Committed;
  illegal.outcome = MigrationOutcome::Committed;
  FO_CHECK(!illegal.validate().ok());

  MigrationRecord stale_event = make_migration();
  MigrationStageEvent future = stage_event(MigrationStage::SourceQuiescing, 2);
  future.generation = MigrationGeneration{9};
  stale_event.stage_events.push_back(future);
  stale_event.stage = MigrationStage::SourceQuiescing;
  FO_CHECK(!stale_event.validate().ok());

  MigrationRecord last_mismatch = make_migration();
  last_mismatch.stage_events.push_back(stage_event(MigrationStage::SourceQuiescing, 2));
  last_mismatch.stage = MigrationStage::Planned;
  FO_CHECK(!last_mismatch.validate().ok());
}

FO_TEST(migration, stale_events_are_identified) {
  MigrationRecord migration = make_migration();
  migration.stage_events.push_back(stage_event(MigrationStage::SourceQuiescing, 10));
  migration.stage = MigrationStage::SourceQuiescing;
  FO_CHECK(is_stale_migration_event(migration, MigrationGeneration{1}, Sequence{9}));
  FO_CHECK(is_stale_migration_event(migration, MigrationGeneration{1}, Sequence{10}));
  FO_CHECK(!is_stale_migration_event(migration, MigrationGeneration{1}, Sequence{11}));
  FO_CHECK(is_stale_migration_event(migration, MigrationGeneration{}, Sequence{11}));
  FO_CHECK(!is_stale_migration_event(migration, MigrationGeneration{2}, Sequence{1}));
}

FO_TEST(migration, analysis_reports_what_is_and_is_not_proven) {
  MigrationRecord migration = make_migration();
  migration.stage_events.push_back(stage_event(MigrationStage::SourceQuiescing, 2));
  migration.stage_events.push_back(stage_event(MigrationStage::StateCaptured, 3));
  migration.stage_events.push_back(stage_event(MigrationStage::TransferStarted, 4));
  migration.stage_events.push_back(stage_event(MigrationStage::TransferComplete, 5));
  migration.stage_events.push_back(stage_event(MigrationStage::DestinationPrepared, 6));
  migration.stage_events.push_back(stage_event(MigrationStage::RestoreStarted, 7));
  migration.stage_events.push_back(stage_event(MigrationStage::RestoreComplete, 8));
  migration.stage = MigrationStage::RestoreComplete;
  migration.outcome = MigrationOutcome::InProgress;
  migration.reason = "rebalance";
  migration.reason_observed = true;
  migration.destination_generation_changed = true;
  migration.revalidation_pending = true;
  migration.requires_rebuild = true;

  PortabilityAssessment portability;
  portability.workload = migration.workload;
  portability.workload_generation = migration.workload_generation;
  portability.destination = migration.destination;
  PortabilityDimensionResult state;
  state.dimension = PortabilityDimension::State;
  state.outcome = PortabilityOutcome::PortableDirect;
  state.precision = Precision::Exact;
  portability.dimensions.push_back(state);
  PortabilityDimensionResult kernel;
  kernel.dimension = PortabilityDimension::Kernel;
  kernel.outcome = PortabilityOutcome::PortableWithRecompile;
  portability.dimensions.push_back(kernel);
  portability.requires_recompile = true;

  MigrationAnalysisInputs inputs;
  inputs.migration = &migration;
  inputs.portability = &portability;
  const MigrationAnalysis analysis = analyze_migration(inputs);

  FO_CHECK(analysis.stage == MigrationStage::RestoreComplete);
  FO_CHECK(analysis.revalidation_outstanding);
  FO_CHECK(analysis.destination_changed_mid_migration);
  FO_CHECK(analysis.adaptation_observed);
  FO_CHECK(analysis.state_portability_proven == Tri::Unknown);
  FO_CHECK(analysis.performance_portability_proven == Tri::Unknown);
  bool mentions_performance = false;
  for (const std::string& conclusion : analysis.conclusions) {
    if (conclusion.find("performance portability is never asserted") != std::string::npos) {
      mentions_performance = true;
    }
  }
  FO_CHECK(mentions_performance);
  FO_CHECK_EQ(analysis.digest(), analyze_migration(inputs).digest());
}

FO_TEST(migration, state_portability_failure_is_named) {
  MigrationRecord migration = make_migration();
  migration.stage = MigrationStage::RolledBack;
  migration.outcome = MigrationOutcome::RolledBack;
  PortabilityAssessment portability;
  portability.workload = migration.workload;
  portability.workload_generation = migration.workload_generation;
  portability.destination = migration.destination;
  PortabilityDimensionResult state;
  state.dimension = PortabilityDimension::State;
  state.outcome = PortabilityOutcome::NotPortableState;
  portability.dimensions.push_back(state);
  MigrationAnalysisInputs inputs;
  inputs.migration = &migration;
  inputs.portability = &portability;
  const MigrationAnalysis analysis = analyze_migration(inputs);
  FO_CHECK(analysis.state_portability_proven == Tri::No);
  FO_CHECK(analysis.outcome == MigrationOutcome::RolledBack);
}

FO_TEST(migration, superseded_migration_is_reported) {
  MigrationRecord migration = make_migration();
  MigrationAnalysisInputs inputs;
  inputs.migration = &migration;
  inputs.superseded_by = MigrationId::unchecked("migration-2");
  const MigrationAnalysis analysis = analyze_migration(inputs);
  FO_CHECK(analysis.superseded);
  FO_CHECK_EQ(analysis.superseded_by.value(), std::string("migration-2"));
  bool mentions = false;
  for (const std::string& conclusion : analysis.conclusions) {
    if (conclusion.find("superseded") != std::string::npos) {
      mentions = true;
    }
  }
  FO_CHECK(mentions);
}

FO_TEST(migration, unobserved_reason_is_never_invented) {
  MigrationRecord migration = make_migration();
  MigrationAnalysisInputs inputs;
  inputs.migration = &migration;
  const MigrationAnalysis analysis = analyze_migration(inputs);
  bool mentions = false;
  for (const std::string& conclusion : analysis.conclusions) {
    if (conclusion.find("does not infer one") != std::string::npos) {
      mentions = true;
    }
  }
  FO_CHECK(mentions);
  FO_CHECK(!analysis.adaptation_observed);
}

FO_TEST(migration, missing_record_is_reported_not_guessed) {
  const MigrationAnalysisInputs inputs;
  const MigrationAnalysis analysis = analyze_migration(inputs);
  FO_CHECK(analysis.precision == Precision::Unknown);
  FO_CHECK_EQ(analysis.conclusions.size(), 1u);
  FO_CHECK(analysis.conclusions.front().find("no migration record") != std::string::npos);
}
