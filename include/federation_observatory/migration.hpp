// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/portability.hpp"

namespace fo {

/// Observed migration stages. The runtime observes these; it never drives them.
enum class MigrationStage : std::uint8_t {
  Planned = 0,
  SourceQuiescing = 1,
  StateCaptured = 2,
  TransferStarted = 3,
  TransferComplete = 4,
  DestinationPrepared = 5,
  RestoreStarted = 6,
  RestoreComplete = 7,
  RevalidationRequired = 8,
  Committed = 9,
  RolledBack = 10,
  Failed = 11,
  OutcomeUnknown = 12,
};

inline constexpr std::size_t kMigrationStageCount = 13;

[[nodiscard]] FO_API std::string_view to_string(MigrationStage stage) noexcept;
[[nodiscard]] FO_API bool parse_migration_stage(std::string_view text, MigrationStage& out) noexcept;
[[nodiscard]] FO_API bool is_terminal_stage(MigrationStage stage) noexcept;
/// Deterministic progress rank; stages that move backwards are regressions unless the
/// migration was explicitly rolled back or failed.
[[nodiscard]] FO_API int migration_stage_rank(MigrationStage stage) noexcept;
/// Legal stage transition under the observed state machine.
[[nodiscard]] FO_API bool is_valid_migration_transition(MigrationStage from, MigrationStage to) noexcept;
/// True when p to is a legal forward step *or* an idempotent re-publication of p from.
[[nodiscard]] FO_API bool is_acceptable_migration_event(MigrationStage from, MigrationStage to) noexcept;

/// Final disposition of a migration observation.
enum class MigrationOutcome : std::uint8_t {
  InProgress = 0,
  Committed = 1,
  RolledBack = 2,
  Failed = 3,
  Unknown = 4,
};

[[nodiscard]] FO_API std::string_view to_string(MigrationOutcome outcome) noexcept;
[[nodiscard]] FO_API bool parse_migration_outcome(std::string_view text, MigrationOutcome& out) noexcept;

/// One observed stage transition, bound to the migration generation that produced it.
struct FO_API MigrationStageEvent {
  MigrationGeneration generation;
  MigrationStage stage = MigrationStage::Planned;
  Sequence sequence;
  TimestampNanos observed_at = 0;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  std::string detail;

  friend bool operator==(const MigrationStageEvent& a, const MigrationStageEvent& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// An observed migration with all the generation bindings required to make it
/// meaningful. A migration record that is missing a binding reports the missing
/// binding rather than a confident conclusion.
struct FO_API MigrationRecord {
  MigrationId id;
  MigrationGeneration generation;
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  FederationId federation;
  FederationGeneration federation_generation;
  WorkloadClassId workload_class;

  /// The migration this record supersedes, if any. Late events tagged with the
  /// superseded generation are rejected instead of mutating this record.
  MigrationId supersedes;
  MigrationGeneration supersedes_generation;

  ClusterId source;
  ClusterGeneration source_generation;
  ClusterEpoch source_epoch;
  ClusterId destination;
  ClusterGeneration destination_generation;
  ClusterEpoch destination_epoch;

  ArtifactId artifact;
  ArtifactGeneration artifact_generation;
  RuntimeId source_runtime;
  RuntimeGeneration source_runtime_generation;
  RuntimeId destination_runtime;
  RuntimeGeneration destination_runtime_generation;

  CompatibilityGeneration compatibility_generation;
  PolicyGeneration policy_generation;
  CapacityGeneration capacity_generation;
  TopologyGeneration topology_generation;

  MigrationStage stage = MigrationStage::Planned;
  MigrationOutcome outcome = MigrationOutcome::InProgress;
  std::vector<MigrationStageEvent> stage_events;

  std::string reason;
  bool reason_observed = false;

  bool state_transfer_known = false;
  std::uint64_t state_transfer_bytes = 0;
  bool downtime_known = false;
  std::uint64_t downtime_micros = 0;

  bool requires_rebuild = false;
  bool requires_recompile = false;
  bool requires_conversion = false;
  bool requires_state_translation = false;
  bool revalidation_pending = false;
  bool destination_generation_changed = false;
  bool changed_effective_capability = false;
  bool changed_slo = false;
  std::string fallback_detail;

  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] const MigrationStageEvent* last_event() const noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Deterministic analysis of one migration: adaptation requirements, capability delta,
/// and — critically — what the observation does and does not prove.
struct FO_API MigrationAnalysis {
  MigrationId migration;
  MigrationGeneration generation;
  WorkloadId workload;
  WorkloadGeneration workload_generation;
  MigrationStage stage = MigrationStage::Planned;
  MigrationOutcome outcome = MigrationOutcome::InProgress;
  bool superseded = false;
  MigrationId superseded_by;

  PortabilityAssessment portability;
  bool requires_adaptation = false;
  bool adaptation_observed = false;
  bool revalidation_outstanding = false;
  bool destination_changed_mid_migration = false;
  bool capability_delta_observed = false;
  std::string capability_delta;
  bool slo_delta_observed = false;

  /// "A workload running after migration does not prove byte-for-byte state
  /// portability." Recorded explicitly so consumers cannot read more than is proven.
  Tri state_portability_proven = Tri::Unknown;
  Tri performance_portability_proven = Tri::Unknown;

  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  Currentness currentness = Currentness::Unknown;
  SnapshotGeneration snapshot_generation;
  std::vector<std::string> conclusions;
  EvidenceList evidence;

  [[nodiscard]] std::string digest() const;
  [[nodiscard]] std::string render() const;
};

struct FO_API MigrationAnalysisInputs {
  const MigrationRecord* migration = nullptr;
  /// The portability assessment produced for this migration's source/destination pair.
  const PortabilityAssessment* portability = nullptr;
  /// Identity of the migration that superseded this one, if any.
  MigrationId superseded_by;
  SnapshotGeneration snapshot_generation;
};

[[nodiscard]] FO_API MigrationAnalysis analyze_migration(const MigrationAnalysisInputs& inputs);

/// True when an incoming event must be rejected because it belongs to a superseded or
/// stale migration generation.
[[nodiscard]] FO_API bool is_stale_migration_event(const MigrationRecord& current,
                                                   MigrationGeneration event_generation,
                                                   Sequence event_sequence) noexcept;

}  // namespace fo
