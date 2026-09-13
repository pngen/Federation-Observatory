// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Migration observation. The runtime observes stages; it never drives them. Every
// observation is bound to the generations that make it meaningful, and a late event
// from a superseded generation is refused rather than applied.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/migration.hpp"

namespace fo {
namespace {

const char* stage_name(MigrationStage stage) {
  switch (stage) {
    case MigrationStage::Planned: return "PLANNED";
    case MigrationStage::SourceQuiescing: return "SOURCE_QUIESCING";
    case MigrationStage::StateCaptured: return "STATE_CAPTURED";
    case MigrationStage::TransferStarted: return "TRANSFER_STARTED";
    case MigrationStage::TransferComplete: return "TRANSFER_COMPLETE";
    case MigrationStage::DestinationPrepared: return "DESTINATION_PREPARED";
    case MigrationStage::RestoreStarted: return "RESTORE_STARTED";
    case MigrationStage::RestoreComplete: return "RESTORE_COMPLETE";
    case MigrationStage::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case MigrationStage::Committed: return "COMMITTED";
    case MigrationStage::RolledBack: return "ROLLED_BACK";
    case MigrationStage::Failed: return "FAILED";
    case MigrationStage::OutcomeUnknown: return "OUTCOME_UNKNOWN";
  }
  return "OUTCOME_UNKNOWN";
}

}  // namespace

std::string_view to_string(MigrationStage stage) noexcept { return stage_name(stage); }

bool parse_migration_stage(std::string_view text, MigrationStage& out) noexcept {
  for (std::size_t i = 0; i < kMigrationStageCount; ++i) {
    const auto candidate = static_cast<MigrationStage>(i);
    if (stage_name(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool is_terminal_stage(MigrationStage stage) noexcept {
  return stage == MigrationStage::Committed || stage == MigrationStage::RolledBack ||
         stage == MigrationStage::Failed;
}

int migration_stage_rank(MigrationStage stage) noexcept {
  switch (stage) {
    case MigrationStage::Planned: return 0;
    case MigrationStage::SourceQuiescing: return 1;
    case MigrationStage::StateCaptured: return 2;
    case MigrationStage::TransferStarted: return 3;
    case MigrationStage::TransferComplete: return 4;
    case MigrationStage::DestinationPrepared: return 5;
    case MigrationStage::RestoreStarted: return 6;
    case MigrationStage::RestoreComplete: return 7;
    case MigrationStage::RevalidationRequired: return 8;
    case MigrationStage::Committed: return 9;
    case MigrationStage::RolledBack: return -1;
    case MigrationStage::Failed: return -1;
    case MigrationStage::OutcomeUnknown: return -1;
  }
  return -1;
}

bool is_valid_migration_transition(MigrationStage from, MigrationStage to) noexcept {
  switch (from) {
    case MigrationStage::Planned:
      return to == MigrationStage::SourceQuiescing || to == MigrationStage::Failed ||
             to == MigrationStage::RolledBack || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::SourceQuiescing:
      return to == MigrationStage::StateCaptured || to == MigrationStage::Failed ||
             to == MigrationStage::RolledBack || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::StateCaptured:
      return to == MigrationStage::TransferStarted || to == MigrationStage::DestinationPrepared ||
             to == MigrationStage::Failed || to == MigrationStage::RolledBack ||
             to == MigrationStage::OutcomeUnknown;
    case MigrationStage::TransferStarted:
      return to == MigrationStage::TransferComplete || to == MigrationStage::Failed ||
             to == MigrationStage::RolledBack || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::TransferComplete:
      return to == MigrationStage::DestinationPrepared || to == MigrationStage::RestoreStarted ||
             to == MigrationStage::Failed || to == MigrationStage::RolledBack ||
             to == MigrationStage::OutcomeUnknown;
    case MigrationStage::DestinationPrepared:
      return to == MigrationStage::RestoreStarted || to == MigrationStage::Failed ||
             to == MigrationStage::RolledBack || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::RestoreStarted:
      return to == MigrationStage::RestoreComplete || to == MigrationStage::Failed ||
             to == MigrationStage::RolledBack || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::RestoreComplete:
      return to == MigrationStage::RevalidationRequired || to == MigrationStage::Committed ||
             to == MigrationStage::RolledBack || to == MigrationStage::Failed ||
             to == MigrationStage::OutcomeUnknown;
    case MigrationStage::RevalidationRequired:
      return to == MigrationStage::Committed || to == MigrationStage::RolledBack ||
             to == MigrationStage::Failed || to == MigrationStage::OutcomeUnknown;
    case MigrationStage::Committed:
    case MigrationStage::RolledBack:
    case MigrationStage::Failed:
      return false;
    case MigrationStage::OutcomeUnknown:
      return to == MigrationStage::Committed || to == MigrationStage::RolledBack ||
             to == MigrationStage::Failed || to == MigrationStage::RevalidationRequired;
  }
  return false;
}

bool is_acceptable_migration_event(MigrationStage from, MigrationStage to) noexcept {
  return from == to || is_valid_migration_transition(from, to);
}

std::string_view to_string(MigrationOutcome outcome) noexcept {
  switch (outcome) {
    case MigrationOutcome::InProgress: return "IN_PROGRESS";
    case MigrationOutcome::Committed: return "COMMITTED";
    case MigrationOutcome::RolledBack: return "ROLLED_BACK";
    case MigrationOutcome::Failed: return "FAILED";
    case MigrationOutcome::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool parse_migration_outcome(std::string_view text, MigrationOutcome& out) noexcept {
  for (int i = 0; i <= static_cast<int>(MigrationOutcome::Unknown); ++i) {
    const auto candidate = static_cast<MigrationOutcome>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool operator==(const MigrationStageEvent& a, const MigrationStageEvent& b) noexcept {
  return a.generation == b.generation && a.stage == b.stage && a.sequence == b.sequence &&
         a.observed_at == b.observed_at && a.precision == b.precision &&
         a.evidence_class == b.evidence_class && a.detail == b.detail;
}

std::string MigrationStageEvent::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"generation", generation.is_set() ? generation.to_string() : "-"});
  rows.push_back({"stage", stage_name(stage)});
  rows.push_back({"sequence", sequence.to_string()});
  rows.push_back({"observed_at", format_timestamp(observed_at)});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"detail", detail.empty() ? "-" : detail});
  return std::string(indent) + "stage_event:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status MigrationRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "migration id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "migration generation is unset", id.value());
  }
  if (workload.empty()) {
    return fail(ErrorCode::InvalidArgument, "migration has no workload", id.value());
  }
  if (!workload_generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "migration workload generation is unset", id.value());
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "migration has no federation", id.value());
  }
  if (source.empty() || destination.empty()) {
    return fail(ErrorCode::InvalidArgument, "migration must name a source and a destination",
                id.value());
  }
  if (source == destination) {
    return fail(ErrorCode::InvalidArgument, "migration source and destination are the same cluster",
                id.value());
  }
  if (stage_events.size() > kMaxStageEventsPerMigration) {
    return fail(ErrorCode::BoundExceeded, "too many migration stage events", id.value());
  }

  Sequence previous_sequence;
  MigrationStage previous_stage = MigrationStage::Planned;
  bool first = true;
  for (const MigrationStageEvent& event : stage_events) {
    if (!event.generation.is_set()) {
      return fail(ErrorCode::InvalidArgument, "stage event has no migration generation", id.value());
    }
    if (event.generation.newer_than(generation)) {
      return fail(ErrorCode::InvalidArgument,
                  "stage event carries a newer migration generation than the record", id.value());
    }
    if (!first) {
      if (event.sequence <= previous_sequence) {
        return fail(ErrorCode::SequenceRegression,
                    "stage event sequence does not increase", id.value() + " " +
                        event.sequence.to_string());
      }
      if (!is_acceptable_migration_event(previous_stage, event.stage)) {
        return fail(ErrorCode::InvalidTransition, "invalid migration stage transition",
                    id.value() + " " + stage_name(previous_stage) + " -> " + stage_name(event.stage));
      }
    }
    previous_sequence = event.sequence;
    previous_stage = event.stage;
    first = false;
  }

  if (!stage_events.empty() && stage_events.back().stage != stage) {
    return fail(ErrorCode::InvalidTransition,
                "recorded migration stage does not match the last stage event",
                id.value() + " record=" + stage_name(stage) + " last_event=" +
                    stage_name(stage_events.back().stage));
  }

  const MigrationOutcome expected = [this] {
    switch (stage) {
      case MigrationStage::Committed: return MigrationOutcome::Committed;
      case MigrationStage::RolledBack: return MigrationOutcome::RolledBack;
      case MigrationStage::Failed: return MigrationOutcome::Failed;
      case MigrationStage::OutcomeUnknown: return MigrationOutcome::Unknown;
      default: return MigrationOutcome::InProgress;
    }
  }();
  if (outcome != expected) {
    return fail(ErrorCode::InvalidTransition, "migration outcome does not match its stage",
                id.value() + " stage=" + stage_name(stage) +
                    " outcome=" + std::string(fo::to_string(outcome)));
  }
  if (supersedes == id && !supersedes.empty()) {
    return fail(ErrorCode::InvalidArgument, "migration supersedes itself", id.value());
  }
  return Status::success();
}

const MigrationStageEvent* MigrationRecord::last_event() const noexcept {
  return stage_events.empty() ? nullptr : &stage_events.back();
}

std::string MigrationRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"workload", workload.value()});
  rows.push_back({"workload_generation", workload_generation.to_string()});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"supersedes", render_id(supersedes.value())});
  rows.push_back({"source", source.value()});
  rows.push_back({"source_generation", source_generation.is_set() ? source_generation.to_string() : "-"});
  rows.push_back({"source_epoch", source_epoch.is_set() ? source_epoch.to_string() : "-"});
  rows.push_back({"destination", destination.value()});
  rows.push_back({"destination_generation",
                  destination_generation.is_set() ? destination_generation.to_string() : "-"});
  rows.push_back({"destination_epoch",
                  destination_epoch.is_set() ? destination_epoch.to_string() : "-"});
  rows.push_back({"artifact", render_id(artifact.value())});
  rows.push_back({"artifact_generation",
                  artifact_generation.is_set() ? artifact_generation.to_string() : "-"});
  rows.push_back({"source_runtime", render_id(source_runtime.value())});
  rows.push_back({"destination_runtime", render_id(destination_runtime.value())});
  rows.push_back({"compatibility_generation",
                  compatibility_generation.is_set() ? compatibility_generation.to_string() : "-"});
  rows.push_back({"policy_generation", policy_generation.is_set() ? policy_generation.to_string() : "-"});
  rows.push_back({"stage", stage_name(stage)});
  rows.push_back({"outcome", std::string(fo::to_string(outcome))});
  rows.push_back({"reason", reason_observed ? (reason.empty() ? "-" : reason) : "NOT_OBSERVED"});
  rows.push_back({"state_transfer_bytes",
                  state_transfer_known ? std::to_string(state_transfer_bytes) : "UNKNOWN"});
  rows.push_back({"downtime_micros", downtime_known ? std::to_string(downtime_micros) : "UNKNOWN"});
  rows.push_back({"requires_rebuild", requires_rebuild ? "yes" : "no"});
  rows.push_back({"requires_recompile", requires_recompile ? "yes" : "no"});
  rows.push_back({"requires_conversion", requires_conversion ? "yes" : "no"});
  rows.push_back({"requires_state_translation", requires_state_translation ? "yes" : "no"});
  rows.push_back({"revalidation_pending", revalidation_pending ? "yes" : "no"});
  rows.push_back({"destination_generation_changed", destination_generation_changed ? "yes" : "no"});
  rows.push_back({"changed_effective_capability", changed_effective_capability ? "yes" : "no"});
  rows.push_back({"changed_slo", changed_slo ? "yes" : "no"});
  rows.push_back({"fallback_detail", fallback_detail.empty() ? "-" : fallback_detail});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  std::string out = std::string(indent) + "migration:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!stage_events.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> event_rows;
    for (const MigrationStageEvent& event : stage_events) {
      event_rows.push_back({event.generation.to_string(), stage_name(event.stage),
                            event.sequence.to_string(), format_timestamp(event.observed_at),
                            std::string(fo::to_string(event.evidence_class)),
                            event.detail.empty() ? "-" : event.detail});
    }
    out += render_table({"generation", "stage", "sequence", "observed_at", "evidence_class", "detail"},
                        event_rows, std::string(indent) + "  ");
    out += '\n';
  }
  return out;
}

bool is_stale_migration_event(const MigrationRecord& current, MigrationGeneration event_generation,
                              Sequence event_sequence) noexcept {
  if (!event_generation.is_set()) {
    return true;
  }
  if (event_generation.older_than(current.generation)) {
    return true;
  }
  if (event_generation.newer_than(current.generation)) {
    return false;
  }
  if (const MigrationStageEvent* last = current.last_event()) {
    return event_sequence <= last->sequence;
  }
  return false;
}

MigrationAnalysis analyze_migration(const MigrationAnalysisInputs& inputs) {
  MigrationAnalysis analysis;
  analysis.snapshot_generation = inputs.snapshot_generation;
  if (inputs.migration == nullptr) {
    analysis.conclusions.emplace_back("no migration record was supplied");
    analysis.precision = Precision::Unknown;
    analysis.evidence_class = EvidenceClass::Unknown;
    return analysis;
  }

  const MigrationRecord& migration = *inputs.migration;
  analysis.migration = migration.id;
  analysis.generation = migration.generation;
  analysis.workload = migration.workload;
  analysis.workload_generation = migration.workload_generation;
  analysis.stage = migration.stage;
  analysis.outcome = migration.outcome;
  analysis.superseded = !inputs.superseded_by.empty();
  analysis.superseded_by = inputs.superseded_by;
  analysis.currentness = migration.currentness;

  if (inputs.portability != nullptr) {
    analysis.portability = *inputs.portability;
  }

  analysis.requires_adaptation = analysis.portability.requires_rebuild ||
                                 analysis.portability.requires_recompile ||
                                 analysis.portability.requires_conversion ||
                                 analysis.portability.requires_state_translation ||
                                 analysis.portability.requires_fallback;

  // Adaptation is only *observed* once the migration has progressed far enough that the
  // adaptation step must already have happened. This is derived, never observed.
  const int rank = migration_stage_rank(migration.stage);
  analysis.adaptation_observed = analysis.requires_adaptation && rank >= 7;
  analysis.revalidation_outstanding =
      migration.revalidation_pending || migration.stage == MigrationStage::RevalidationRequired;
  analysis.destination_changed_mid_migration = migration.destination_generation_changed;

  analysis.capability_delta_observed =
      (!migration.source_runtime.empty() || !migration.destination_runtime.empty()) &&
      (migration.source_runtime != migration.destination_runtime ||
       migration.source_runtime_generation != migration.destination_runtime_generation ||
       migration.source_generation != migration.destination_generation);
  if (analysis.capability_delta_observed) {
    analysis.capability_delta = "runtime " + render_id(migration.source_runtime.value()) + " gen " +
                                (migration.source_runtime_generation.is_set()
                                     ? migration.source_runtime_generation.to_string()
                                     : std::string("-")) +
                                " -> " + render_id(migration.destination_runtime.value()) + " gen " +
                                (migration.destination_runtime_generation.is_set()
                                     ? migration.destination_runtime_generation.to_string()
                                     : std::string("-")) +
                                "; cluster generation " +
                                (migration.source_generation.is_set()
                                     ? migration.source_generation.to_string()
                                     : std::string("-")) +
                                " -> " +
                                (migration.destination_generation.is_set()
                                     ? migration.destination_generation.to_string()
                                     : std::string("-"));
  }
  analysis.slo_delta_observed = migration.changed_slo;

  // State portability: a committed migration proves the workload runs at the
  // destination. It does not prove that the transferred state was byte-for-byte intact.
  if (const PortabilityDimensionResult* state =
          analysis.portability.find(PortabilityDimension::State)) {
    if (state->outcome == PortabilityOutcome::PortableDirect &&
        state->precision == Precision::Exact && migration.outcome == MigrationOutcome::Committed) {
      analysis.state_portability_proven = Tri::Yes;
    } else if (state->outcome == PortabilityOutcome::NotPortableState) {
      analysis.state_portability_proven = Tri::No;
    } else {
      analysis.state_portability_proven = Tri::Unknown;
    }
  } else {
    analysis.state_portability_proven = Tri::Unknown;
  }
  analysis.performance_portability_proven = Tri::Unknown;

  analysis.precision = weakest(migration.stamp.precision, analysis.portability.precision);
  analysis.evidence_class = weaker(migration.stamp.evidence_class, analysis.portability.evidence_class);
  for (const EvidenceRef& ref : migration.evidence.items()) {
    const Status s = analysis.evidence.add(ref);
    (void)s;
  }
  for (const EvidenceRef& ref : analysis.portability.evidence.items()) {
    const Status s = analysis.evidence.add(ref);
    (void)s;
  }

  analysis.conclusions.emplace_back("migration " + migration.id.value() + " generation " +
                                    migration.generation.to_string() + " is at stage " +
                                    stage_name(migration.stage) + " with outcome " +
                                    std::string(fo::to_string(migration.outcome)));
  if (migration.reason_observed && !migration.reason.empty()) {
    analysis.conclusions.emplace_back("the source stated the reason for this migration: " +
                                      migration.reason);
  } else {
    analysis.conclusions.emplace_back(
        "no reason for this migration was observed; the runtime does not infer one");
  }
  if (analysis.superseded) {
    analysis.conclusions.emplace_back("this migration was superseded by " +
                                      inputs.superseded_by.value() +
                                      "; late events bearing its generation are refused");
  }
  if (analysis.destination_changed_mid_migration) {
    analysis.conclusions.emplace_back(
        "the destination cluster generation changed during the migration, which forces "
        "revalidation of the destination evidence");
  }
  if (analysis.requires_adaptation) {
    analysis.conclusions.emplace_back(
        std::string("the source/destination pair requires adaptation: ") +
        (analysis.portability.requires_rebuild ? "rebuild " : "") +
        (analysis.portability.requires_recompile ? "recompile " : "") +
        (analysis.portability.requires_conversion ? "conversion " : "") +
        (analysis.portability.requires_state_translation ? "state-translation " : "") +
        (analysis.portability.requires_fallback ? "fallback" : ""));
  } else if (analysis.portability.dimensions.empty()) {
    analysis.conclusions.emplace_back(
        "no portability assessment accompanies this migration, so adaptation needs are unknown");
  }
  if (analysis.revalidation_outstanding) {
    analysis.conclusions.emplace_back(
        "revalidation is outstanding: the destination's post-migration evidence has not been "
        "confirmed");
  }
  switch (analysis.state_portability_proven) {
    case Tri::Yes:
      analysis.conclusions.emplace_back(
          "the state dimension of the accompanying portability assessment is directly portable "
          "with exact evidence; this is the only basis on which state portability is asserted");
      break;
    case Tri::No:
      analysis.conclusions.emplace_back(
          "state portability failed: the captured state format is not restorable at the "
          "destination");
      break;
    case Tri::Unknown:
      analysis.conclusions.emplace_back(
          "state portability is not proven: a workload running after migration does not "
          "demonstrate byte-for-byte state portability");
      break;
  }
  analysis.conclusions.emplace_back(
      "performance portability is never asserted by Federation Observatory; it is not measured "
      "here");

  return analysis;
}

std::string MigrationAnalysis::digest() const { return digest_text(render()); }

std::string MigrationAnalysis::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"migration", render_id(migration.value())});
  rows.push_back({"generation", generation.is_set() ? generation.to_string() : "-"});
  rows.push_back({"workload", render_id(workload.value())});
  rows.push_back({"workload_generation",
                  workload_generation.is_set() ? workload_generation.to_string() : "-"});
  rows.push_back({"stage", stage_name(stage)});
  rows.push_back({"outcome", std::string(fo::to_string(outcome))});
  rows.push_back({"superseded", superseded ? "yes" : "no"});
  rows.push_back({"superseded_by", render_id(superseded_by.value())});
  rows.push_back({"requires_adaptation", requires_adaptation ? "yes" : "no"});
  rows.push_back({"adaptation_observed", adaptation_observed ? "yes" : "no"});
  rows.push_back({"revalidation_outstanding", revalidation_outstanding ? "yes" : "no"});
  rows.push_back({"destination_changed_mid_migration",
                  destination_changed_mid_migration ? "yes" : "no"});
  rows.push_back({"capability_delta_observed", capability_delta_observed ? "yes" : "no"});
  rows.push_back({"capability_delta", capability_delta.empty() ? "-" : capability_delta});
  rows.push_back({"slo_delta_observed", slo_delta_observed ? "yes" : "no"});
  rows.push_back({"state_portability_proven", std::string(fo::to_string(state_portability_proven))});
  rows.push_back(
      {"performance_portability_proven", std::string(fo::to_string(performance_portability_proven))});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"snapshot_generation",
                  snapshot_generation.is_set() ? snapshot_generation.to_string() : "-"});
  std::string out = "migration analysis:\n";
  out += render_table({"field", "value"}, rows, "  ");
  out += "\n  conclusions:";
  for (const std::string& conclusion : conclusions) {
    out += "\n    - ";
    out += conclusion;
  }
  out += "\n\n  portability:\n";
  out += indent_block(portability.render("    "), "");
  return out;
}

}  // namespace fo
