// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/bounds.hpp"
#include "federation_observatory/capacity.hpp"
#include "federation_observatory/compatibility.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"
#include "federation_observatory/snapshot.hpp"
#include "federation_observatory/state_store.hpp"

namespace fo {

/// Authority context attached to every publication. All of it is required: an
/// observation without a publisher boot identity, a coordinator epoch and a sequence
/// cannot be ordered, cannot be replayed safely, and cannot be attributed.
struct FO_API PublicationContext {
  PublisherId publisher;
  BootGeneration boot;
  CoordinatorEpoch coordinator_epoch;
  FederationId federation;
  FederationGeneration federation_generation;
  Sequence sequence;
  TimestampNanos observed_at = 0;
  Precision precision = Precision::Exact;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  Provenance provenance = Provenance::Unknown;
  EvidenceGeneration evidence_generation;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render() const;
};

/// What the runtime did with an *accepted* publication.
///
/// A rejection is never represented here: it is reported as a classified Error, so that
/// "the coordinator processed my request" and "the coordinator accepted my observation"
/// can never be confused. Note that "accepted but did not change state" (Duplicate,
/// Superseded) is still a distinct, successful outcome.
enum class IngestDisposition : std::uint8_t {
  Applied = 0,     ///< State was updated.
  Duplicate = 1,   ///< Byte-identical re-publication; suppressed, no state change.
  Superseded = 2,  ///< Valid but older than current state for its key; retained as history only.
  Deferred = 3,    ///< Accepted into the bounded queue; not yet applied.
};

[[nodiscard]] FO_API std::string_view to_string(IngestDisposition disposition) noexcept;

/// Result of one accepted ingest. Always reports the resulting per-publisher watermark so
/// a publisher can tell how far the coordinator has consumed its stream.
struct FO_API IngestResult {
  IngestDisposition disposition = IngestDisposition::Applied;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  Sequence watermark;
  /// Number of sequence numbers skipped between the previous accepted event and this
  /// one. Non-zero means evidence is known-incomplete and is surfaced, never hidden.
  std::uint64_t sequence_gap = 0;

  [[nodiscard]] bool applied() const noexcept { return disposition == IngestDisposition::Applied; }
  /// True when the publication was accepted, whether or not it changed state.
  [[nodiscard]] bool changed_state() const noexcept {
    return disposition == IngestDisposition::Applied;
  }
  [[nodiscard]] std::string render() const;
};

/// Configuration of an observatory instance.
struct FO_API ObservatoryConfig {
  Bounds bounds;
  /// Retain placement/migration history up to the bounds. Disabling history keeps only
  /// the latest record per key and is intended for memory-constrained observers.
  bool retain_history = true;
};

/// Cumulative counters. Exposed for backpressure visibility and benchmarks.
struct FO_API ObservatoryStats {
  std::uint64_t publications_applied = 0;
  std::uint64_t publications_duplicate = 0;
  std::uint64_t publications_superseded = 0;
  std::uint64_t publications_rejected = 0;
  std::uint64_t publications_fenced = 0;
  std::uint64_t sequence_gaps_observed = 0;
  std::uint64_t sequence_regressions_rejected = 0;
  std::uint64_t queue_rejections = 0;
  std::uint64_t snapshots_created = 0;
  std::uint64_t state_saves = 0;
  std::uint64_t state_loads = 0;
  std::uint64_t bounds_rejections = 0;
  std::uint64_t placements_recorded = 0;
  std::uint64_t migrations_recorded = 0;
  std::uint64_t clusters_registered = 0;
  std::uint64_t cluster_retirements = 0;
  std::uint64_t publisher_registrations = 0;
  std::uint64_t publisher_fences = 0;

  [[nodiscard]] std::string render() const;
};

/// The federation observation runtime.
///
/// Thread safety: every public method is safe to call concurrently from any thread.
/// Internally the model is protected by a single shared mutex; queries copy immutable
/// handles out of it and do all analysis outside the lock. No socket, filesystem,
/// callback or analysis work ever runs while a lock is held.
class FO_API FederationObservatory {
 public:
  explicit FederationObservatory(ObservatoryConfig config = {});
  ~FederationObservatory();

  FederationObservatory(const FederationObservatory&) = delete;
  FederationObservatory& operator=(const FederationObservatory&) = delete;
  FederationObservatory(FederationObservatory&&) = delete;
  FederationObservatory& operator=(FederationObservatory&&) = delete;

  // ------------------------------------------------------------------
  // Publisher authority
  // ------------------------------------------------------------------

  /// Register a publisher boot identity. A boot identity that was fenced stays fenced
  /// forever; a replacement process must present a fresh BootGeneration.
  [[nodiscard]] Result<IngestResult> register_publisher(const PublicationContext& ctx);
  /// Permanently revoke a boot identity. Idempotent. Reason is recorded.
  [[nodiscard]] Status fence_publisher(const PublisherId& publisher, BootGeneration boot,
                                       std::string reason);
  [[nodiscard]] bool publisher_is_live(const PublisherId& publisher, BootGeneration boot) const;

  // ------------------------------------------------------------------
  // Registration (structural state)
  // ------------------------------------------------------------------

  [[nodiscard]] Result<IngestResult> register_federation(const PublicationContext& ctx,
                                                         FederationRecord record);
  [[nodiscard]] Result<IngestResult> register_site(const PublicationContext& ctx, SiteRecord record);
  [[nodiscard]] Result<IngestResult> register_cluster(const PublicationContext& ctx,
                                                      ClusterRecord record);
  [[nodiscard]] Result<IngestResult> register_accelerator_class(
      const PublicationContext& ctx, AcceleratorClassRecord record);
  [[nodiscard]] Result<IngestResult> register_runtime(const PublicationContext& ctx,
                                                      RuntimeRecord record);
  [[nodiscard]] Result<IngestResult> register_backend(const PublicationContext& ctx,
                                                      BackendRecord record);
  [[nodiscard]] Result<IngestResult> register_domain(const PublicationContext& ctx,
                                                     DomainRecord record);
  [[nodiscard]] Result<IngestResult> register_policy(const PublicationContext& ctx,
                                                     PolicyRecord record);
  [[nodiscard]] Result<IngestResult> register_artifact(const PublicationContext& ctx,
                                                       ArtifactRecord record);
  [[nodiscard]] Result<IngestResult> register_workload_class(const PublicationContext& ctx,
                                                             WorkloadClassRecord record);
  [[nodiscard]] Result<IngestResult> register_workload(const PublicationContext& ctx,
                                                       WorkloadRecord record);

  /// Retire a cluster. Its history is preserved; its dynamic evidence stops being
  /// current immediately and its generation is bumped so late evidence is rejected.
  [[nodiscard]] Result<IngestResult> retire_cluster(const PublicationContext& ctx,
                                                    const ClusterId& cluster,
                                                    ClusterGeneration generation,
                                                    std::string reason);

  // ------------------------------------------------------------------
  // Dynamic publications
  // ------------------------------------------------------------------

  [[nodiscard]] Result<IngestResult> publish_capability(const PublicationContext& ctx,
                                                        const ClusterId& cluster,
                                                        ClusterGeneration cluster_generation,
                                                        AcceleratorCapabilityGeneration capability_generation,
                                                        CapabilitySet capabilities);
  [[nodiscard]] Result<IngestResult> publish_capacity(const PublicationContext& ctx,
                                                      const ClusterId& cluster,
                                                      ClusterGeneration cluster_generation,
                                                      CapacityGeneration capacity_generation,
                                                      std::vector<CapacityPool> pools);
  [[nodiscard]] Result<IngestResult> publish_placement(const PublicationContext& ctx,
                                                       PlacementRecord record);
  [[nodiscard]] Result<IngestResult> publish_migration(const PublicationContext& ctx,
                                                       MigrationRecord record);
  [[nodiscard]] Result<IngestResult> publish_migration_stage(const PublicationContext& ctx,
                                                             const MigrationId& migration,
                                                             MigrationGeneration generation,
                                                             MigrationStage stage,
                                                             std::string detail);
  [[nodiscard]] Result<IngestResult> publish_portability(const PublicationContext& ctx,
                                                         PortabilityAssessment record);

  // ------------------------------------------------------------------
  // Queries - all return immutable values derived from an immutable snapshot
  // ------------------------------------------------------------------

  [[nodiscard]] SnapshotHandle snapshot() const;
  [[nodiscard]] SnapshotHandle snapshot_for(const FederationId& federation) const;

  [[nodiscard]] Result<PlacementExplanation> explain_placement(const PlacementId& placement) const;
  [[nodiscard]] Result<PlacementExplanation> explain_latest_placement(const WorkloadId& workload) const;
  [[nodiscard]] Result<RejectionExplanation> explain_rejection(const PlacementId& placement,
                                                               const ClusterId& candidate) const;

  [[nodiscard]] Result<StrandedCapacityReport> stranded_capacity(
      const StrandedCapacityRequest& request) const;
  [[nodiscard]] Result<FragmentationFinding> fragmentation(const FederationId& federation,
                                                           const WorkloadClassId& workload_class,
                                                           ResourceKind kind) const;
  [[nodiscard]] Result<CompatibilityAssessment> compatibility(const ClusterId& cluster,
                                                              const WorkloadId& workload) const;
  [[nodiscard]] Result<PortabilityAssessment> portability(const WorkloadId& workload,
                                                          const ClusterId& destination) const;
  [[nodiscard]] Result<PortabilityAssessment> evaluate_portability(const WorkloadId& workload,
                                                                   const ClusterId& destination) const;
  [[nodiscard]] Result<MigrationAnalysis> migration_analysis(const MigrationId& migration) const;
  [[nodiscard]] Result<MismatchAnalysis> mismatch_analysis(
      const MismatchAnalysisRequest& request) const;
  [[nodiscard]] Result<DriftReport> drift(const DriftRequest& request) const;
  [[nodiscard]] Result<FederationRecord> federation(const FederationId& id) const;

  // ------------------------------------------------------------------
  // Durability and lifecycle
  // ------------------------------------------------------------------

  /// Serialize durable state. Structural and historical only; dynamic liveness and
  /// capacity are intentionally excluded.
  [[nodiscard]] Status save_state(const std::string& path) const;
  /// Atomically replace a state file with the current durable state.
  [[nodiscard]] Status checkpoint(const std::string& path) const;
  /// Load durable state. On any validation failure nothing is applied.
  [[nodiscard]] Status load_state(const std::string& path);
  /// Apply an already-decoded durable state with full validation and all-or-nothing
  /// semantics. Advances the coordinator epoch by one.
  [[nodiscard]] Status restore(const DurableState& state);

  [[nodiscard]] const Bounds& bounds() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;
  [[nodiscard]] SnapshotGeneration snapshot_generation() const noexcept;
  [[nodiscard]] ObservatoryStats stats() const;

  /// Explicitly mark all dynamic evidence as requiring revalidation. Called on restore;
  /// also callable by an operator that suspects its upstream evidence is invalid.
  [[nodiscard]] Status invalidate_dynamic_evidence(std::string reason);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fo
