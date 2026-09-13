// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// The observation state engine: publisher authority, generation fencing, deterministic
// ingest, immutable snapshots, and the query surface.
//
// Locking. Exactly one std::shared_mutex protects the whole model. Analysts take a
// shared lock only long enough to copy the records they need, release it, and then
// analyse. No socket I/O, filesystem I/O, callback or analysis pass ever runs while the
// lock is held, so the only ordering rule is: never re-enter the observatory from inside
// a locked region. With one lock there is no lock-ordering cycle to get wrong.

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/records_codec.hpp"

namespace fo {
namespace {

using PublisherKey = std::pair<PublisherId, BootGeneration>;

struct PublisherAuthority {
  PublisherId publisher;
  BootGeneration boot;
  CoordinatorEpoch epoch;
  FederationId federation;
  Sequence watermark;
  TimestampNanos registered_at = 0;
  TimestampNanos last_publication_at = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t gaps = 0;
  bool live = false;
  bool fenced = false;
  std::string fenced_reason;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
};

struct PortabilityKey {
  WorkloadId workload;
  ClusterId destination;
  friend bool operator<(const PortabilityKey& a, const PortabilityKey& b) {
    if (a.workload != b.workload) return a.workload < b.workload;
    return a.destination < b.destination;
  }
};

/// Outcome of the authority gate that every publication passes through first.
struct AuthorityCheck {
  PublisherAuthority* authority = nullptr;
  bool duplicate_sequence = false;
  bool failed = false;
  Error failure;
};

/// Digest of the *published* content of a record: the runtime's own annotations
/// (currentness, the observation stamp, and accumulated evidence) are excluded, so a
/// byte-identical re-publication is recognised as a duplicate rather than as a change.
template <class Record>
std::string published_digest(Record record) {
  record.currentness = Currentness::Unknown;
  record.stamp = ObservationStamp{};
  record.evidence = EvidenceList{};
  return codec::digest_record(record);
}

}  // namespace

std::string_view to_string(IngestDisposition disposition) noexcept {
  switch (disposition) {
    case IngestDisposition::Applied: return "APPLIED";
    case IngestDisposition::Duplicate: return "DUPLICATE";
    case IngestDisposition::Superseded: return "SUPERSEDED";
    case IngestDisposition::Deferred: return "DEFERRED";
  }
  return "REJECTED";
}

std::string IngestResult::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"disposition", std::string(fo::to_string(disposition))});
  rows.push_back({"code", std::string(fo::to_string(code))});
  rows.push_back({"detail", detail.empty() ? "-" : detail});
  rows.push_back({"watermark", watermark.to_string()});
  rows.push_back({"sequence_gap", std::to_string(sequence_gap)});
  return render_table({"field", "value"}, rows, "  ");
}

std::string ObservatoryStats::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"publications_applied", std::to_string(publications_applied)});
  rows.push_back({"publications_duplicate", std::to_string(publications_duplicate)});
  rows.push_back({"publications_superseded", std::to_string(publications_superseded)});
  rows.push_back({"publications_rejected", std::to_string(publications_rejected)});
  rows.push_back({"publications_fenced", std::to_string(publications_fenced)});
  rows.push_back({"sequence_gaps_observed", std::to_string(sequence_gaps_observed)});
  rows.push_back({"sequence_regressions_rejected", std::to_string(sequence_regressions_rejected)});
  rows.push_back({"queue_rejections", std::to_string(queue_rejections)});
  rows.push_back({"snapshots_created", std::to_string(snapshots_created)});
  rows.push_back({"state_saves", std::to_string(state_saves)});
  rows.push_back({"state_loads", std::to_string(state_loads)});
  rows.push_back({"bounds_rejections", std::to_string(bounds_rejections)});
  rows.push_back({"placements_recorded", std::to_string(placements_recorded)});
  rows.push_back({"migrations_recorded", std::to_string(migrations_recorded)});
  rows.push_back({"clusters_registered", std::to_string(clusters_registered)});
  rows.push_back({"cluster_retirements", std::to_string(cluster_retirements)});
  rows.push_back({"publisher_registrations", std::to_string(publisher_registrations)});
  rows.push_back({"publisher_fences", std::to_string(publisher_fences)});
  return render_table({"counter", "value"}, rows, "");
}

Status PublicationContext::validate() const {
  if (publisher.empty()) {
    return fail(ErrorCode::InvalidArgument, "publication context has no publisher");
  }
  if (!boot.is_set()) {
    return fail(ErrorCode::InvalidArgument, "publication context has no publisher boot identity");
  }
  if (!coordinator_epoch.is_set()) {
    return fail(ErrorCode::InvalidArgument, "publication context has no coordinator epoch");
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "publication context has no federation");
  }
  if (sequence.is_zero()) {
    return fail(ErrorCode::InvalidArgument,
                "publication sequence must be at least 1; sequence 0 is reserved");
  }
  if (!is_valid_identifier(publisher.value()) || !is_valid_identifier(federation.value())) {
    return fail(ErrorCode::InvalidArgument, "publication context carries a malformed identifier");
  }
  return Status::success();
}

std::string PublicationContext::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"publisher", publisher.value()});
  rows.push_back({"boot", boot.to_string()});
  rows.push_back({"coordinator_epoch", coordinator_epoch.to_string()});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"sequence", sequence.to_string()});
  rows.push_back({"observed_at", format_timestamp(observed_at)});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"provenance", std::string(fo::to_string(provenance))});
  rows.push_back({"evidence_generation",
                  evidence_generation.is_set() ? evidence_generation.to_string() : "-"});
  return render_table({"field", "value"}, rows, "  ");
}

class FederationObservatory::Impl {
 public:
  explicit Impl(ObservatoryConfig config_in) : config(std::move(config_in)) {
    bounds = config.bounds;
    const Status valid = bounds.validate();
    if (!valid.ok()) {
      bounds = default_bounds();
    }
  }

  ObservatoryConfig config;
  Bounds bounds;

  mutable std::shared_mutex mutex;

  std::map<PublisherKey, PublisherAuthority> publishers;
  std::uint64_t save_sequence = 0;
  CoordinatorEpoch coordinator_epoch_{1};
  SnapshotGeneration snapshot_generation_{1};

  std::map<FederationId, FederationRecord> federations;
  std::map<SiteId, SiteRecord> sites;
  std::map<ClusterId, ClusterRecord> clusters;
  std::map<AcceleratorClassId, AcceleratorClassRecord> accelerator_classes;
  std::map<RuntimeId, RuntimeRecord> runtimes;
  std::map<BackendId, BackendRecord> backends;
  std::map<DomainId, DomainRecord> domains;
  std::map<PolicyId, PolicyRecord> policies;
  std::map<ArtifactId, ArtifactRecord> artifacts;
  std::map<WorkloadClassId, WorkloadClassRecord> workload_classes;
  std::map<WorkloadId, WorkloadRecord> workloads;
  std::map<PlacementId, PlacementRecord> placements;
  std::map<WorkloadId, PlacementId> latest_placement;
  std::map<MigrationId, MigrationRecord> migrations;
  std::map<WorkloadId, MigrationId> latest_migration;
  std::map<WorkloadId, MigrationId> superseded_migrations;
  std::map<PortabilityKey, PortabilityAssessment> portability;
  std::vector<AggregateFindingRecord> aggregate_findings;

  mutable ObservatoryStats stats;

  [[nodiscard]] ObservationStamp stamp_for(const PublicationContext& ctx) const {
    ObservationStamp stamp;
    stamp.publisher = ctx.publisher;
    stamp.publisher_boot = ctx.boot;
    stamp.coordinator_epoch = ctx.coordinator_epoch;
    stamp.evidence_generation = ctx.evidence_generation;
    stamp.sequence = ctx.sequence;
    stamp.observed_at = ctx.observed_at != 0 ? ctx.observed_at : now_unix_nanos();
    stamp.precision = ctx.precision;
    stamp.evidence_class = ctx.evidence_class;
    stamp.provenance = ctx.provenance;
    return stamp;
  }

  /// Refuse a publication. The runtime reports refusals as classified Errors so that a
  /// caller can never mistake "processed" for "accepted".
  [[nodiscard]] Error refuse(ErrorCode code, std::string detail, PublisherAuthority* authority,
                             bool fenced = false) {
    if (authority != nullptr) {
      authority->rejected += 1;
    }
    stats.publications_rejected += 1;
    if (fenced) {
      stats.publications_fenced += 1;
    }
    return Error(code, std::move(detail));
  }

  [[nodiscard]] AuthorityCheck check_authority(const PublicationContext& ctx) {
    AuthorityCheck check;
    const Status ctx_status = ctx.validate();
    if (!ctx_status.ok()) {
      check.failed = true;
      check.failure = refuse(ctx_status.code(), ctx_status.error().message(), nullptr);
      return check;
    }
    const auto it = publishers.find(PublisherKey{ctx.publisher, ctx.boot});
    if (it == publishers.end()) {
      check.failed = true;
      check.failure = refuse(ErrorCode::StaleBoot,
                             "publisher boot identity is not registered with this coordinator "
                             "incarnation; register it first",
                             nullptr, true);
      return check;
    }
    PublisherAuthority& authority = it->second;
    check.authority = &authority;
    if (authority.fenced) {
      check.failed = true;
      check.failure = refuse(ErrorCode::FencedPublisher,
                             authority.fenced_reason.empty()
                                 ? std::string("publisher boot identity has been fenced")
                                 : "publisher boot identity has been fenced: " +
                                       authority.fenced_reason,
                             &authority, true);
      return check;
    }
    if (ctx.coordinator_epoch != coordinator_epoch_) {
      check.failed = true;
      check.failure = refuse(ErrorCode::StaleEpoch,
                             "publication carries coordinator epoch " +
                                 ctx.coordinator_epoch.to_string() +
                                 "; this coordinator is at epoch " + coordinator_epoch_.to_string(),
                             &authority);
      return check;
    }
    if (ctx.sequence < authority.watermark) {
      stats.sequence_regressions_rejected += 1;
      check.failed = true;
      check.failure = refuse(ErrorCode::SequenceRegression,
                             "sequence " + ctx.sequence.to_string() +
                                 " is older than the accepted watermark " +
                                 authority.watermark.to_string(),
                             &authority);
      return check;
    }
    check.duplicate_sequence = ctx.sequence == authority.watermark && !authority.watermark.is_zero();
    return check;
  }

  [[nodiscard]] IngestResult finish(const PublicationContext& ctx, PublisherAuthority& authority,
                                    IngestDisposition disposition, std::string detail = {}) {
    std::uint64_t gap = 0;
    if (ctx.sequence > authority.watermark) {
      gap = ctx.sequence.value() - authority.watermark.value() - 1;
      authority.watermark = ctx.sequence;
    }
    if (gap != 0) {
      stats.sequence_gaps_observed += 1;
      authority.gaps += 1;
    }
    authority.last_publication_at = ctx.observed_at != 0 ? ctx.observed_at : now_unix_nanos();
    IngestResult result;
    result.disposition = disposition;
    result.code = ErrorCode::Ok;
    result.detail = std::move(detail);
    result.watermark = authority.watermark;
    result.sequence_gap = gap;
    if (disposition == IngestDisposition::Applied) {
      authority.accepted += 1;
      stats.publications_applied += 1;
    } else if (disposition == IngestDisposition::Duplicate) {
      authority.duplicates += 1;
      stats.publications_duplicate += 1;
    } else if (disposition == IngestDisposition::Superseded) {
      stats.publications_superseded += 1;
    }
    return result;
  }

  /// Resolve the federation named by a publication context and verify its generation.
  [[nodiscard]] Status check_federation(const PublicationContext& ctx,
                                        const FederationRecord*& record) {
    const auto it = federations.find(ctx.federation);
    if (it == federations.end()) {
      return fail(ErrorCode::NotFound, "federation is not registered", ctx.federation.value());
    }
    record = &it->second;
    if (ctx.federation_generation.is_set() &&
        ctx.federation_generation != it->second.generation) {
      return fail(ErrorCode::StaleGeneration,
                  "publication targets federation generation " +
                      ctx.federation_generation.to_string() + " but the current generation is " +
                      it->second.generation.to_string(),
                  ctx.federation.value());
    }
    return Status::success();
  }

  /// Generic structural registration: validate, check cross references, fence on
  /// generation, deduplicate by content digest, and apply.
  template <class Record, class Map, class Extra, class LimitFn, class GenerationFn>
  Result<IngestResult> register_structural(const PublicationContext& ctx, Record record, Map& map,
                                           const char* label, Extra extra, LimitFn limit_fn,
                                           GenerationFn generation_of) {
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;

    const Status valid = record.validate();
    if (!valid.ok()) {
      return refuse(valid.code(), valid.error().message() + " (" + valid.error().detail() + ")",
                    &authority);
    }
    const Status extra_status = extra(record);
    if (!extra_status.ok()) {
      return refuse(extra_status.code(),
                    extra_status.error().message() + " (" + extra_status.error().detail() + ")",
                    &authority);
    }

    const std::string incoming_digest = published_digest(record);
    const auto it = map.find(record.id);
    if (it != map.end() && published_digest(it->second) == incoming_digest) {
      return finish(ctx, authority, IngestDisposition::Duplicate,
                    std::string("identical ") + label + " re-publication was suppressed");
    }
    if (check.duplicate_sequence) {
      // A publisher that re-uses an accepted sequence number for different content has
      // either replayed a corrupted stream or lost its counter. The observation cannot be
      // ordered, so it is refused rather than applied.
      return refuse(ErrorCode::Conflict,
                    std::string("sequence ") + ctx.sequence.to_string() +
                        " was already accepted with different " + label + " content",
                    &authority);
    }
    if (it == map.end()) {
      const std::size_t limit = limit_fn();
      if (map.size() >= limit) {
        stats.bounds_rejections += 1;
        return refuse(ErrorCode::BoundExceeded,
                      std::string("the ") + label + " bound is exhausted", &authority);
      }
      record.currentness = Currentness::Current;
      record.stamp = stamp_for(ctx);
      map.emplace(record.id, std::move(record));
      snapshot_generation_ = snapshot_generation_.next();
      return finish(ctx, authority, IngestDisposition::Applied);
    }

    const auto stored_generation = generation_of(it->second);
    const auto incoming_generation = generation_of(record);
    if (stored_generation.newer_than(incoming_generation)) {
      return finish(ctx, authority, IngestDisposition::Superseded,
                    std::string("stored ") + label + " generation " +
                        stored_generation.to_string() + " is newer than the published " +
                        incoming_generation.to_string());
    }
    record.currentness = Currentness::Current;
    record.stamp = stamp_for(ctx);
    it->second = std::move(record);
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  // ------------------------------------------------------------------
  // Publisher authority
  // ------------------------------------------------------------------

  Result<IngestResult> register_publisher(const PublicationContext& ctx) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    const Status ctx_status = ctx.validate();
    if (!ctx_status.ok()) {
      return ctx_status.error();
    }
    const PublisherKey key{ctx.publisher, ctx.boot};
    const auto it = publishers.find(key);
    if (it != publishers.end()) {
      PublisherAuthority& authority = it->second;
      if (authority.fenced) {
        return refuse(ErrorCode::FencedPublisher,
                      "publisher boot identity " + ctx.boot.to_string() + " of publisher " +
                          ctx.publisher.value() +
                          " was fenced and can never be re-registered; a replacement process must "
                          "present a fresh boot identity",
                      &authority, true);
      }
      if (ctx.coordinator_epoch != coordinator_epoch_) {
        return refuse(ErrorCode::StaleEpoch,
                      "registration carries coordinator epoch " +
                          ctx.coordinator_epoch.to_string(),
                      &authority);
      }
      if (ctx.sequence < authority.watermark) {
        stats.sequence_regressions_rejected += 1;
        return refuse(ErrorCode::SequenceRegression, "registration sequence regressed", &authority);
      }
      authority.live = true;
      authority.epoch = ctx.coordinator_epoch;
      authority.federation = ctx.federation;
      authority.evidence_class = ctx.evidence_class;
      return finish(ctx, authority, IngestDisposition::Duplicate,
                    "publisher boot identity was already registered and remains live");
    }
    if (publishers.size() >= bounds.max_publishers) {
      stats.bounds_rejections += 1;
      return Error(ErrorCode::BoundExceeded, "publisher bound is exhausted");
    }
    std::size_t in_federation = 0;
    for (const auto& entry : publishers) {
      if (entry.second.federation == ctx.federation) {
        ++in_federation;
      }
    }
    if (in_federation >= bounds.max_publishers_per_federation) {
      stats.bounds_rejections += 1;
      return Error(ErrorCode::BoundExceeded, "per-federation publisher bound is exhausted",
                   ctx.federation.value());
    }
    PublisherAuthority authority;
    authority.publisher = ctx.publisher;
    authority.boot = ctx.boot;
    authority.epoch = ctx.coordinator_epoch;
    authority.federation = ctx.federation;
    authority.watermark = Sequence{};
    authority.registered_at = ctx.observed_at != 0 ? ctx.observed_at : now_unix_nanos();
    authority.last_publication_at = authority.registered_at;
    authority.live = true;
    authority.evidence_class = ctx.evidence_class;
    PublisherAuthority& stored = publishers.emplace(key, std::move(authority)).first->second;
    stats.publisher_registrations += 1;
    if (ctx.sequence > stored.watermark) {
      stored.watermark = ctx.sequence;
    }
    IngestResult result;
    result.disposition = IngestDisposition::Applied;
    result.code = ErrorCode::Ok;
    result.detail = "publisher boot identity registered";
    result.watermark = stored.watermark;
    result.sequence_gap = stored.watermark.value() > 1 ? stored.watermark.value() - 1 : 0;
    stored.accepted += 1;
    stats.publications_applied += 1;
    snapshot_generation_ = snapshot_generation_.next();
    return result;
  }

  Status fence_publisher(const PublisherId& publisher, BootGeneration boot, std::string reason) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    if (publisher.empty() || !boot.is_set()) {
      return fail(ErrorCode::InvalidArgument, "fence requires a publisher and a boot identity");
    }
    const PublisherKey key{publisher, boot};
    const auto it = publishers.find(key);
    if (it == publishers.end()) {
      // Fencing a boot identity this incarnation has never seen is still recorded, so
      // that a replay from that identity can never be accepted later.
      PublisherAuthority authority;
      authority.publisher = publisher;
      authority.boot = boot;
      authority.epoch = coordinator_epoch_;
      authority.fenced = true;
      authority.fenced_reason = std::move(reason);
      authority.live = false;
      publishers.emplace(key, std::move(authority));
      stats.publisher_fences += 1;
      return Status::success();
    }
    PublisherAuthority& authority = it->second;
    if (!authority.fenced) {
      authority.fenced = true;
      authority.fenced_reason = std::move(reason);
      stats.publisher_fences += 1;
    }
    authority.live = false;
    // Every record this boot identity produced stops being current.
    for (auto& entry : clusters) {
      if (entry.second.stamp.publisher == publisher && entry.second.stamp.publisher_boot == boot) {
        if (entry.second.currentness == Currentness::Current) {
          entry.second.currentness = Currentness::Stale;
        }
      }
    }
    for (auto& entry : federations) {
      if (entry.second.stamp.publisher == publisher && entry.second.stamp.publisher_boot == boot &&
          entry.second.currentness == Currentness::Current) {
        entry.second.currentness = Currentness::Stale;
      }
    }
    for (auto& entry : sites) {
      if (entry.second.stamp.publisher == publisher && entry.second.stamp.publisher_boot == boot &&
          entry.second.currentness == Currentness::Current) {
        entry.second.currentness = Currentness::Stale;
      }
    }
    for (auto& entry : domains) {
      if (entry.second.stamp.publisher == publisher && entry.second.stamp.publisher_boot == boot &&
          entry.second.currentness == Currentness::Current) {
        entry.second.currentness = Currentness::Stale;
      }
    }
    snapshot_generation_ = snapshot_generation_.next();
    return Status::success();
  }

  [[nodiscard]] bool publisher_is_live(const PublisherId& publisher, BootGeneration boot) const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    const auto it = publishers.find(PublisherKey{publisher, boot});
    return it != publishers.end() && it->second.live && !it->second.fenced;
  }

  // ------------------------------------------------------------------
  // Structural registration
  // ------------------------------------------------------------------

  Result<IngestResult> register_federation(const PublicationContext& ctx, FederationRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), federations, "federation",
        [](const FederationRecord&) { return Status::success(); },
        [this] { return bounds.max_federations; },
        [](const FederationRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_site(const PublicationContext& ctx, SiteRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), sites, "site",
        [this](const SiteRecord& candidate) -> Status {
          if (federations.find(candidate.federation) == federations.end()) {
            return fail(ErrorCode::NotFound, "site references an unregistered federation",
                        candidate.federation.value());
          }
          return Status::success();
        },
        [this] { return bounds.max_sites; },
        [](const SiteRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_accelerator_class(const PublicationContext& ctx,
                                                  AcceleratorClassRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), accelerator_classes, "accelerator class",
        [](const AcceleratorClassRecord&) { return Status::success(); },
        [this] { return bounds.max_accelerator_classes; },
        [](const AcceleratorClassRecord& r) { return r.capability_generation; });
  }

  Result<IngestResult> register_runtime(const PublicationContext& ctx, RuntimeRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), runtimes, "runtime",
        [](const RuntimeRecord&) { return Status::success(); },
        [this] { return bounds.max_runtimes; },
        [](const RuntimeRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_backend(const PublicationContext& ctx, BackendRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), backends, "backend",
        [this](const BackendRecord& candidate) -> Status {
          if (runtimes.find(candidate.runtime) == runtimes.end()) {
            return fail(ErrorCode::NotFound, "backend references an unregistered runtime",
                        candidate.runtime.value());
          }
          return Status::success();
        },
        [this] { return bounds.max_backends; },
        [](const BackendRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_domain(const PublicationContext& ctx, DomainRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), domains, "domain",
        [this](const DomainRecord& candidate) -> Status {
          if (federations.find(candidate.federation) == federations.end()) {
            return fail(ErrorCode::NotFound, "domain references an unregistered federation",
                        candidate.federation.value());
          }
          for (const ClusterId& id : candidate.clusters) {
            if (clusters.find(id) == clusters.end()) {
              return fail(ErrorCode::NotFound, "domain references an unregistered cluster",
                          id.value());
            }
          }
          return Status::success();
        },
        [this] { return bounds.max_domains; },
        [](const DomainRecord& r) { return r.topology_generation; });
  }

  Result<IngestResult> register_policy(const PublicationContext& ctx, PolicyRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), policies, "policy",
        [](const PolicyRecord&) { return Status::success(); },
        [this] { return bounds.max_policies; },
        [](const PolicyRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_artifact(const PublicationContext& ctx, ArtifactRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), artifacts, "artifact",
        [](const ArtifactRecord&) { return Status::success(); },
        [this] { return bounds.max_artifacts; },
        [](const ArtifactRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_workload_class(const PublicationContext& ctx,
                                               WorkloadClassRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), workload_classes, "workload class",
        [](const WorkloadClassRecord&) { return Status::success(); },
        [this] { return bounds.max_workload_classes; },
        [](const WorkloadClassRecord&) { return FederationGeneration{}; });
  }

  Result<IngestResult> register_workload(const PublicationContext& ctx, WorkloadRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    return register_structural(
        ctx, std::move(record), workloads, "workload",
        [this](const WorkloadRecord& candidate) -> Status {
          if (workload_classes.find(candidate.workload_class) == workload_classes.end()) {
            return fail(ErrorCode::NotFound, "workload references an unregistered workload class",
                        candidate.workload_class.value());
          }
          if (!candidate.artifact.empty() && artifacts.find(candidate.artifact) == artifacts.end()) {
            return fail(ErrorCode::NotFound, "workload references an unregistered artifact",
                        candidate.artifact.value());
          }
          if (!candidate.policy.empty() && policies.find(candidate.policy) == policies.end()) {
            return fail(ErrorCode::NotFound, "workload references an unregistered policy",
                        candidate.policy.value());
          }
          for (const AcceleratorClassId& id : candidate.acceptable_accelerator_classes) {
            if (accelerator_classes.find(id) == accelerator_classes.end()) {
              return fail(ErrorCode::NotFound,
                          "workload references an unregistered accelerator class", id.value());
            }
          }
          return Status::success();
        },
        [this] { return bounds.max_workloads; },
        [](const WorkloadRecord& r) { return r.generation; });
  }

  Result<IngestResult> register_cluster(const PublicationContext& ctx, ClusterRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    const ClusterId cluster_id = record.id;
    const ClusterGeneration incoming_generation = record.generation;
    ClusterGeneration previous_generation;
    const auto previous = clusters.find(cluster_id);
    if (previous != clusters.end()) {
      previous_generation = previous->second.generation;
    }
    Result<IngestResult> outcome = register_structural(
        ctx, std::move(record), clusters, "cluster",
        [this](const ClusterRecord& candidate) -> Status {
          if (federations.find(candidate.federation) == federations.end()) {
            return fail(ErrorCode::NotFound, "cluster references an unregistered federation",
                        candidate.federation.value());
          }
          if (sites.find(candidate.site) == sites.end()) {
            return fail(ErrorCode::NotFound, "cluster references an unregistered site",
                        candidate.site.value());
          }
          for (const AcceleratorClassId& id : candidate.accelerator_classes) {
            if (accelerator_classes.find(id) == accelerator_classes.end()) {
              return fail(ErrorCode::NotFound,
                          "cluster references an unregistered accelerator class", id.value());
            }
          }
          for (const RuntimeId& id : candidate.runtimes) {
            if (runtimes.find(id) == runtimes.end()) {
              return fail(ErrorCode::NotFound, "cluster references an unregistered runtime",
                          id.value());
            }
          }
          for (const BackendId& id : candidate.backends) {
            if (backends.find(id) == backends.end()) {
              return fail(ErrorCode::NotFound, "cluster references an unregistered backend",
                          id.value());
            }
          }
          if (!candidate.domain.empty() && domains.find(candidate.domain) == domains.end()) {
            return fail(ErrorCode::NotFound, "cluster references an unregistered domain",
                        candidate.domain.value());
          }
          return Status::success();
        },
        [this] { return bounds.max_clusters; },
        [](const ClusterRecord& r) { return r.generation; });
    if (outcome.ok() && outcome.value().disposition == IngestDisposition::Applied &&
        previous_generation.is_set() && incoming_generation.newer_than(previous_generation)) {
      // Advancing a cluster's generation means a new incarnation of the cluster agent.
      // Its capacity and readiness were observed for the previous incarnation and are
      // therefore not evidence about this one.
      const auto it = clusters.find(cluster_id);
      if (it != clusters.end()) {
        it->second.capacity_pools.clear();
        it->second.readiness = Readiness::Unknown;
        const Status note = it->second.evidence.add(
            Provenance::DerivedAnalysis, it->second.stamp.evidence_class, "observatory",
            Precision::Derived,
            "cluster generation advanced from " + previous_generation.to_string() + " to " +
                incoming_generation.to_string() +
                "; dynamic capacity and readiness evidence was cleared and must be republished");
        (void)note;
        snapshot_generation_ = snapshot_generation_.next();
      }
    }
    return outcome;
  }

  Result<IngestResult> retire_cluster(const PublicationContext& ctx, const ClusterId& cluster,
                                      ClusterGeneration generation, std::string reason) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const auto it = clusters.find(cluster);
    if (it == clusters.end()) {
      return refuse(ErrorCode::NotFound, "cluster is not registered", &authority);
    }
    ClusterRecord& record = it->second;
    if (generation.is_set() && generation.older_than(record.generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "retirement targets cluster generation " + generation.to_string() +
                        " but the current generation is " + record.generation.to_string(),
                    &authority);
    }
    if (record.currentness == Currentness::Retired) {
      return finish(ctx, authority, IngestDisposition::Duplicate, "cluster was already retired");
    }
    record.generation = generation.is_set() ? generation : record.generation.next();
    record.currentness = Currentness::Retired;
    record.readiness = Readiness::Retired;
    record.capacity_generation = record.capacity_generation.next();
    record.stamp = stamp_for(ctx);
    const Status note = record.evidence.add(Provenance::ClusterController, ctx.evidence_class,
                                            ctx.publisher.value(), ctx.precision,
                                            "cluster retired: " + reason);
    (void)note;
    stats.cluster_retirements += 1;
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied,
                  "cluster retired; its history is preserved and late evidence for the previous "
                  "generation is refused");
  }

  // ------------------------------------------------------------------
  // Dynamic publications
  // ------------------------------------------------------------------

  Result<IngestResult> publish_capability(const PublicationContext& ctx, const ClusterId& cluster,
                                          ClusterGeneration cluster_generation,
                                          AcceleratorCapabilityGeneration capability_generation,
                                          CapabilitySet capabilities) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const FederationRecord* federation = nullptr;
    Status federation_status = check_federation(ctx, federation);
    if (!federation_status.ok()) {
      return refuse(federation_status.code(), federation_status.error().message(), &authority);
    }
    const auto it = clusters.find(cluster);
    if (it == clusters.end()) {
      return refuse(ErrorCode::NotFound, "cluster is not registered", &authority);
    }
    ClusterRecord& record = it->second;
    if (record.currentness == Currentness::Retired) {
      return refuse(ErrorCode::Conflict, "cluster is retired and cannot receive evidence",
                    &authority);
    }
    if (!capability_generation.is_set()) {
      return refuse(ErrorCode::InvalidArgument,
                    "capability publication requires a capability generation", &authority);
    }
    if (cluster_generation.is_set() && cluster_generation.older_than(record.generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "capability publication targets cluster generation " +
                        cluster_generation.to_string() + " but the current generation is " +
                        record.generation.to_string(),
                    &authority);
    }
    if (capability_generation.older_than(record.capability_generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "capability generation " + capability_generation.to_string() +
                        " is older than the published capability generation " +
                        record.capability_generation.to_string(),
                    &authority);
    }
    // A cluster registration may declare the capability generation it is about to
    // publish under while carrying no capability content yet. Establishing a generation
    // for the first time is not a change; changing content that was already published
    // under a fixed generation is.
    const bool generation_established = !record.capabilities.empty();
    if (capability_generation == record.capability_generation && generation_established) {
      CapabilitySet previous = record.capabilities;
      const bool identical = previous.size() == capabilities.size() &&
                             std::equal(previous.entries().begin(), previous.entries().end(),
                                        capabilities.entries().begin(),
                                        [](const CapabilityEntry& a, const CapabilityEntry& b) {
                                          return a == b;
                                        });
      if (identical) {
        return finish(ctx, authority, IngestDisposition::Duplicate,
                      "identical capability publication for the same generation");
      }
      return refuse(ErrorCode::Conflict,
                    "capability content changed without advancing the capability generation; a "
                    "capability change must be published under a new generation so that dependent "
                    "conclusions can be invalidated",
                    &authority);
    }
    if (cluster_generation.is_set() && cluster_generation.newer_than(record.generation)) {
      record.generation = cluster_generation;
    }
    for (const CapabilityEntry& entry : capabilities.entries()) {
      if (!entry.key.valid()) {
        return refuse(ErrorCode::InvalidArgument, "capability key is not valid", &authority);
      }
    }
    record.capabilities = std::move(capabilities);
    record.capability_generation = capability_generation;
    record.evidence_generation = ctx.evidence_generation;
    record.currentness = Currentness::Current;
    record.stamp = stamp_for(ctx);
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  Result<IngestResult> publish_capacity(const PublicationContext& ctx, const ClusterId& cluster,
                                        ClusterGeneration cluster_generation,
                                        CapacityGeneration capacity_generation,
                                        std::vector<CapacityPool> pools) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const FederationRecord* federation = nullptr;
    Status federation_status = check_federation(ctx, federation);
    if (!federation_status.ok()) {
      return refuse(federation_status.code(), federation_status.error().message(), &authority);
    }
    const auto it = clusters.find(cluster);
    if (it == clusters.end()) {
      return refuse(ErrorCode::NotFound, "cluster is not registered", &authority);
    }
    ClusterRecord& record = it->second;
    if (record.currentness == Currentness::Retired) {
      return refuse(ErrorCode::Conflict, "cluster is retired and cannot receive evidence",
                    &authority);
    }
    if (!capacity_generation.is_set()) {
      return refuse(ErrorCode::InvalidArgument,
                    "capacity publication requires a capacity generation", &authority);
    }
    if (pools.size() > bounds::kMaxPoolsPerCluster) {
      stats.bounds_rejections += 1;
      return refuse(ErrorCode::BoundExceeded, "capacity publication carries too many pools",
                    &authority);
    }
    if (cluster_generation.is_set() && cluster_generation.older_than(record.generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "capacity publication targets cluster generation " +
                        cluster_generation.to_string() + " but the current generation is " +
                        record.generation.to_string(),
                    &authority);
    }
    if (capacity_generation.older_than(record.capacity_generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "capacity generation " + capacity_generation.to_string() +
                        " is older than the published capacity generation " +
                        record.capacity_generation.to_string(),
                    &authority);
    }

    std::vector<ResourcePoolId> pool_ids;
    pool_ids.reserve(pools.size());
    for (const CapacityPool& pool : pools) {
      if (pool.pool_id.empty()) {
        return refuse(ErrorCode::InvalidArgument, "capacity pool has no identifier", &authority);
      }
      const Status ledger_status = pool.ledger.validate();
      if (!ledger_status.ok()) {
        return refuse(ErrorCode::CapacityInconsistent,
                      "capacity pool " + pool.pool_id.value() + " does not satisfy the accounting "
                      "identity: " + ledger_status.error().message(),
                      &authority);
      }
      if (pool.kind == ResourceKind::Accelerator && pool.accelerator_class.empty()) {
        return refuse(ErrorCode::InvalidArgument,
                      "accelerator pool " + pool.pool_id.value() + " names no accelerator class",
                      &authority);
      }
      if (!pool.accelerator_class.empty() &&
          accelerator_classes.find(pool.accelerator_class) == accelerator_classes.end()) {
        return refuse(ErrorCode::NotFound,
                      "capacity pool references an unregistered accelerator class",
                      &authority);
      }
      pool_ids.push_back(pool.pool_id);
    }
    std::sort(pool_ids.begin(), pool_ids.end());
    if (std::adjacent_find(pool_ids.begin(), pool_ids.end()) != pool_ids.end()) {
      return refuse(ErrorCode::InvalidArgument, "capacity publication repeats a pool identifier",
                    &authority);
    }

    // Compare the incoming ledger totals against the stored ones for duplicate detection.
    std::uint64_t incoming_nominal = 0;
    for (const CapacityPool& pool : pools) {
      if (!checked_add(incoming_nominal, pool.ledger.nominal, incoming_nominal)) {
        return refuse(ErrorCode::CapacityInconsistent, "capacity total overflowed", &authority);
      }
    }
    // As with capabilities: a cluster registration may declare the capacity generation it
    // is about to publish under while carrying no capacity yet. Establishing a generation
    // for the first time is not a change.
    const bool capacity_generation_established = !record.capacity_pools.empty();
    if (capacity_generation == record.capacity_generation && capacity_generation_established) {
      std::uint64_t stored_nominal = 0;
      bool overflow = false;
      for (const CapacityPool& pool : record.capacity_pools) {
        if (!checked_add(stored_nominal, pool.ledger.nominal, stored_nominal)) {
          overflow = true;
          break;
        }
      }
      if (!overflow && stored_nominal == incoming_nominal &&
          record.capacity_pools.size() == pools.size()) {
        std::vector<CapacityPool> sorted_incoming = pools;
        std::vector<CapacityPool> sorted_stored = record.capacity_pools;
        std::sort(sorted_incoming.begin(), sorted_incoming.end());
        std::sort(sorted_stored.begin(), sorted_stored.end());
        bool identical = true;
        for (std::size_t i = 0; i < sorted_incoming.size(); ++i) {
          if (!(sorted_incoming[i] == sorted_stored[i])) {
            identical = false;
            break;
          }
        }
        if (identical) {
          return finish(ctx, authority, IngestDisposition::Duplicate,
                        "identical capacity publication for the same generation");
        }
      }
      return refuse(ErrorCode::Conflict,
                    "capacity content changed without advancing the capacity generation; a capacity "
                    "change must be published under a new generation so that stranded-capacity "
                    "conclusions can be recomputed rather than quietly invalidated",
                    &authority);
    }

    std::vector<CapacityPool> accepted = std::move(pools);
    for (CapacityPool& pool : accepted) {
      pool.precision = ctx.precision;
      pool.evidence_class = ctx.evidence_class;
      pool.evidence.set_limit(bounds.max_evidence_per_finding);
      if (pool.generation.is_unset()) {
        pool.generation = capacity_generation;
      }
      const Status note = pool.evidence.add(ctx.provenance, ctx.evidence_class,
                                            ctx.publisher.value(), ctx.precision,
                                            "capacity published under generation " +
                                                capacity_generation.to_string());
      (void)note;
    }
    std::sort(accepted.begin(), accepted.end());
    if (cluster_generation.is_set() && cluster_generation.newer_than(record.generation)) {
      record.generation = cluster_generation;
    }
    record.capacity_pools = std::move(accepted);
    record.capacity_generation = capacity_generation;
    record.evidence_generation = ctx.evidence_generation;
    record.currentness = Currentness::Current;
    record.stamp = stamp_for(ctx);
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  Result<IngestResult> publish_placement(const PublicationContext& ctx, PlacementRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const FederationRecord* federation = nullptr;
    Status federation_status = check_federation(ctx, federation);
    if (!federation_status.ok()) {
      return refuse(federation_status.code(), federation_status.error().message(), &authority);
    }
    const Status valid = record.validate();
    if (!valid.ok()) {
      return refuse(valid.code(),
                    valid.error().message() + " (" + valid.error().detail() + ")", &authority);
    }
    if (record.federation != ctx.federation) {
      return refuse(ErrorCode::InvalidArgument,
                    "placement names federation " + record.federation.value() +
                        " but was published under " + ctx.federation.value(),
                    &authority);
    }
    if (workloads.find(record.workload) == workloads.end()) {
      return refuse(ErrorCode::NotFound, "placement references an unregistered workload",
                    &authority);
    }
    if (clusters.find(record.selected) == clusters.end()) {
      return refuse(ErrorCode::NotFound, "placement references an unregistered cluster",
                    &authority);
    }
    for (const CandidateObservation& candidate : record.candidates) {
      if (clusters.find(candidate.cluster) == clusters.end()) {
        return refuse(ErrorCode::NotFound,
                      "placement candidate references an unregistered cluster",
                      &authority);
      }
    }

    const auto existing = placements.find(record.id);
    if (existing != placements.end() &&
        published_digest(existing->second) == published_digest(record)) {
      return finish(ctx, authority, IngestDisposition::Duplicate,
                    "identical placement re-publication was suppressed");
    }
    if (check.duplicate_sequence) {
      return refuse(ErrorCode::Conflict,
                    "sequence " + ctx.sequence.to_string() +
                        " was already accepted with different placement content",
                    &authority);
    }
    if (existing != placements.end()) {
      if (existing->second.generation.newer_than(record.generation)) {
        return finish(ctx, authority, IngestDisposition::Superseded,
                      "stored placement generation " + existing->second.generation.to_string() +
                          " is newer than the published generation " +
                          record.generation.to_string() + "; the newer record was not overwritten");
      }
      if (existing->second.generation == record.generation) {
        return refuse(ErrorCode::Conflict,
                      "placement content changed without advancing the placement generation",
                      &authority);
      }
    }

    if (placements.size() >= bounds.max_placement_history) {
      stats.bounds_rejections += 1;
      return refuse(ErrorCode::BoundExceeded, "placement history bound is exhausted", &authority);
    }

    // A placement for a workload whose latest placement is newer is retained as history
    // but is not allowed to become the current placement.
    bool supersedes_latest = false;
    const auto latest = latest_placement.find(record.workload);
    if (latest != latest_placement.end()) {
      const auto stored = placements.find(latest->second);
      if (stored != placements.end()) {
        const bool stored_is_newer =
            stored->second.stamp.observed_at > record.stamp.observed_at ||
            (stored->second.stamp.observed_at == record.stamp.observed_at &&
             stored->second.generation.newer_than(record.generation));
        supersedes_latest = stored_is_newer;
      }
    }

    record.stamp = stamp_for(ctx);
    record.currentness = supersedes_latest ? Currentness::Stale : Currentness::Current;
    const Status note = record.evidence.add(
        ctx.provenance, ctx.evidence_class, ctx.publisher.value(), ctx.precision,
        supersedes_latest
            ? "placement retained as history; a newer placement already exists for this workload"
            : "placement accepted as the current placement for this workload generation");
    (void)note;
    const PlacementId id = record.id;
    const WorkloadId placement_workload = record.workload;
    placements[id] = std::move(record);
    if (!supersedes_latest) {
      latest_placement[placement_workload] = id;
    } else {
      latest_placement.emplace(placement_workload, id);
    }
    stats.placements_recorded += 1;
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority,
                  supersedes_latest ? IngestDisposition::Superseded : IngestDisposition::Applied,
                  supersedes_latest
                      ? "placement stored as history; the newer placement remains current"
                      : std::string());
  }

  Result<IngestResult> publish_migration(const PublicationContext& ctx, MigrationRecord record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const FederationRecord* federation = nullptr;
    Status federation_status = check_federation(ctx, federation);
    if (!federation_status.ok()) {
      return refuse(federation_status.code(), federation_status.error().message(), &authority);
    }
    const Status valid = record.validate();
    if (!valid.ok()) {
      return refuse(valid.code(),
                    valid.error().message() + " (" + valid.error().detail() + ")", &authority);
    }
    if (record.federation != ctx.federation) {
      return refuse(ErrorCode::InvalidArgument,
                    "migration names a different federation than the publication context",
                    &authority);
    }
    if (workloads.find(record.workload) == workloads.end()) {
      return refuse(ErrorCode::NotFound, "migration references an unregistered workload",
                    &authority);
    }
    if (clusters.find(record.source) == clusters.end() ||
        clusters.find(record.destination) == clusters.end()) {
      return refuse(ErrorCode::NotFound, "migration references an unregistered cluster", &authority);
    }

    const auto existing = migrations.find(record.id);
    if (existing != migrations.end() &&
        published_digest(existing->second) == published_digest(record)) {
      return finish(ctx, authority, IngestDisposition::Duplicate,
                    "identical migration re-publication was suppressed");
    }
    if (check.duplicate_sequence) {
      return refuse(ErrorCode::Conflict,
                    "sequence " + ctx.sequence.to_string() +
                        " was already accepted with different migration content",
                    &authority);
    }
    if (existing != migrations.end()) {
      if (existing->second.generation.newer_than(record.generation)) {
        return finish(ctx, authority, IngestDisposition::Superseded,
                      "stored migration generation " + existing->second.generation.to_string() +
                          " is newer than the published generation " +
                          record.generation.to_string());
      }
      if (existing->second.generation == record.generation) {
        return refuse(ErrorCode::Conflict,
                      "migration content changed without advancing the migration generation",
                      &authority);
      }
    }

    if (migrations.size() >= bounds.max_migration_history) {
      stats.bounds_rejections += 1;
      return refuse(ErrorCode::BoundExceeded, "migration history bound is exhausted", &authority);
    }

    record.stamp = stamp_for(ctx);
    record.currentness = Currentness::Current;
    const MigrationId id = record.id;
    const WorkloadId workload = record.workload;
    const MigrationId supersedes = record.supersedes;
    migrations[id] = std::move(record);
    if (!supersedes.empty()) {
      const auto previous = migrations.find(supersedes);
      if (previous != migrations.end() && previous->second.currentness == Currentness::Current) {
        previous->second.currentness = Currentness::Stale;
      }
      superseded_migrations[workload] = supersedes;
    }
    latest_migration[workload] = id;
    stats.migrations_recorded += 1;
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  Result<IngestResult> publish_migration_stage(const PublicationContext& ctx,
                                               const MigrationId& migration,
                                               MigrationGeneration generation, MigrationStage stage,
                                               std::string detail) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const auto it = migrations.find(migration);
    if (it == migrations.end()) {
      return refuse(ErrorCode::NotFound, "migration is not registered", &authority);
    }
    MigrationRecord& record = it->second;
    if (!generation.is_set()) {
      return refuse(ErrorCode::InvalidArgument, "migration stage event requires a generation",
                    &authority);
    }
    if (generation.older_than(record.generation)) {
      return refuse(ErrorCode::StaleGeneration,
                    "stage event carries migration generation " + generation.to_string() +
                        " which is older than the current migration generation " +
                        record.generation.to_string() +
                        "; late events from a superseded generation are refused",
                    &authority);
    }
    if (generation.newer_than(record.generation)) {
      return refuse(ErrorCode::InvalidArgument,
                    "stage event carries migration generation " + generation.to_string() +
                        " which has not been registered for this migration",
                    &authority);
    }
    if (record.currentness == Currentness::Retired) {
      return refuse(ErrorCode::Conflict, "migration is retired", &authority);
    }
    // Once a migration has been superseded, its generation is historical: further events
    // for it are refused rather than applied to a record that no longer describes the
    // workload's current move.
    const auto superseding = superseded_migrations.find(record.workload);
    if (superseding != superseded_migrations.end() && superseding->second == migration) {
      return refuse(ErrorCode::StaleGeneration,
                    "this migration was superseded and no longer accepts stage events",
                    &authority);
    }
    if (const MigrationStageEvent* last = record.last_event()) {
      if (ctx.sequence <= last->sequence) {
        stats.sequence_regressions_rejected += 1;
        return refuse(ErrorCode::SequenceRegression,
                      "stage event sequence " + ctx.sequence.to_string() +
                          " does not follow the last recorded sequence " + last->sequence.to_string(),
                      &authority);
      }
    }
    if (!is_acceptable_migration_event(record.stage, stage)) {
      return refuse(ErrorCode::InvalidTransition,
                    std::string("illegal migration stage transition ") +
                        std::string(fo::to_string(record.stage)) + " -> " +
                        std::string(fo::to_string(stage)),
                    &authority);
    }
    if (is_terminal_stage(record.stage)) {
      if (record.stage == stage) {
        return finish(ctx, authority, IngestDisposition::Duplicate,
                      "terminal migration stage re-published; the commit is recorded once");
      }
      return refuse(ErrorCode::InvalidTransition,
                    "migration is already terminal at stage " +
                        std::string(fo::to_string(record.stage)),
                    &authority);
    }

    MigrationStageEvent event;
    event.generation = generation;
    event.stage = stage;
    event.sequence = ctx.sequence;
    event.observed_at = ctx.observed_at != 0 ? ctx.observed_at : now_unix_nanos();
    event.precision = ctx.precision;
    event.evidence_class = ctx.evidence_class;
    event.detail = std::move(detail);
    if (record.stage_events.size() >= kMaxStageEventsPerMigration) {
      stats.bounds_rejections += 1;
      return refuse(ErrorCode::BoundExceeded, "migration stage history bound is exhausted",
                    &authority);
    }
    record.stage_events.push_back(std::move(event));
    record.stage = stage;
    switch (stage) {
      case MigrationStage::Committed: record.outcome = MigrationOutcome::Committed; break;
      case MigrationStage::RolledBack: record.outcome = MigrationOutcome::RolledBack; break;
      case MigrationStage::Failed: record.outcome = MigrationOutcome::Failed; break;
      case MigrationStage::OutcomeUnknown: record.outcome = MigrationOutcome::Unknown; break;
      case MigrationStage::RevalidationRequired: record.revalidation_pending = true; break;
      default: record.outcome = MigrationOutcome::InProgress; break;
    }
    record.stamp = stamp_for(ctx);
    record.currentness = Currentness::Current;
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  Result<IngestResult> publish_portability(const PublicationContext& ctx,
                                           PortabilityAssessment record) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    AuthorityCheck check = check_authority(ctx);
    if (check.failed) {
      return check.failure;
    }
    PublisherAuthority& authority = *check.authority;
    const Status valid = record.validate();
    if (!valid.ok()) {
      return refuse(valid.code(),
                    valid.error().message() + " (" + valid.error().detail() + ")", &authority);
    }
    if (workloads.find(record.workload) == workloads.end()) {
      return refuse(ErrorCode::NotFound, "portability record references an unregistered workload",
                    &authority);
    }
    if (clusters.find(record.destination) == clusters.end()) {
      return refuse(ErrorCode::NotFound,
                    "portability record references an unregistered destination cluster", &authority);
    }
    const PortabilityKey key{record.workload, record.destination};
    const auto existing = portability.find(key);
    if (existing != portability.end()) {
      const std::string incoming_digest = record.digest();
      const std::string stored_digest = existing->second.digest();
      if (incoming_digest == stored_digest) {
        return finish(ctx, authority, IngestDisposition::Duplicate,
                      "identical portability assessment was suppressed");
      }
      if (existing->second.stamp.observed_at > record.stamp.observed_at ||
          (existing->second.stamp.observed_at == record.stamp.observed_at &&
           existing->second.stamp.sequence > ctx.sequence)) {
        return finish(ctx, authority, IngestDisposition::Superseded,
                      "a newer portability assessment is already stored for this pair");
      }
    }
    if (portability.size() >= bounds.max_portability_records && existing == portability.end()) {
      stats.bounds_rejections += 1;
      return refuse(ErrorCode::BoundExceeded, "portability record bound is exhausted", &authority);
    }

    record.stamp = stamp_for(ctx);
    record.currentness = Currentness::Current;
    if (record.federation.empty()) {
      record.federation = ctx.federation;
    }
    portability[key] = std::move(record);
    snapshot_generation_ = snapshot_generation_.next();
    return finish(ctx, authority, IngestDisposition::Applied);
  }

  // ------------------------------------------------------------------
  // Snapshots
  // ------------------------------------------------------------------

  [[nodiscard]] SnapshotHandle build_snapshot(const FederationId& restrict_to) const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    auto snapshot = std::make_shared<FederationSnapshot>();
    stats.snapshots_created += 1;
    snapshot->coordinator_epoch = coordinator_epoch_;
    snapshot->snapshot_generation = snapshot_generation_;
    snapshot->created_at = now_unix_nanos();

    std::size_t estimate = 0;
    const std::size_t budget = bounds.max_snapshot_bytes;
    const auto account = [&estimate, budget](std::size_t records) {
      estimate += records * 192;
      return estimate <= budget;
    };

    std::size_t total_records = 0;
    const auto count_all = [&total_records](const auto& map) { total_records += map.size(); };
    count_all(federations);
    count_all(sites);
    count_all(clusters);
    count_all(accelerator_classes);
    count_all(runtimes);
    count_all(backends);
    count_all(domains);
    count_all(policies);
    count_all(artifacts);
    count_all(workload_classes);
    count_all(workloads);
    count_all(placements);
    count_all(migrations);
    total_records += portability.size();
    total_records += publishers.size();
    snapshot->records_total = total_records;

    bool compatible = true;
    const auto append = [&](const auto& map, auto& target, auto matches) {
      for (const auto& entry : map) {
        if (!matches(entry.second)) {
          continue;
        }
        if (!compatible) {
          continue;
        }
        target.push_back(entry.second);
        if (!account(1)) {
          compatible = false;
          snapshot->truncated = true;
        }
      }
    };

    const bool all = restrict_to.empty();
    const auto federation_matches = [&](const FederationId& id) {
      return all || id == restrict_to;
    };

    append(federations, snapshot->federations,
           [&](const FederationRecord& r) { return federation_matches(r.id); });
    append(sites, snapshot->sites,
           [&](const SiteRecord& r) { return federation_matches(r.federation); });
    append(clusters, snapshot->clusters,
           [&](const ClusterRecord& r) { return federation_matches(r.federation); });
    append(accelerator_classes, snapshot->accelerator_classes, [](const AcceleratorClassRecord&) {
      return true;
    });
    append(runtimes, snapshot->runtimes, [](const RuntimeRecord&) { return true; });
    append(backends, snapshot->backends, [](const BackendRecord&) { return true; });
    append(domains, snapshot->domains,
           [&](const DomainRecord& r) { return federation_matches(r.federation); });
    append(policies, snapshot->policies, [](const PolicyRecord&) { return true; });
    append(artifacts, snapshot->artifacts, [](const ArtifactRecord&) { return true; });
    append(workload_classes, snapshot->workload_classes, [](const WorkloadClassRecord&) {
      return true;
    });
    append(workloads, snapshot->workloads, [](const WorkloadRecord&) { return true; });
    append(placements, snapshot->placements,
           [&](const PlacementRecord& r) { return federation_matches(r.federation); });
    append(migrations, snapshot->migrations,
           [&](const MigrationRecord& r) { return federation_matches(r.federation); });
    for (const auto& entry : portability) {
      if (!compatible) {
        break;
      }
      if (!all && !entry.second.federation.empty() && entry.second.federation != restrict_to) {
        continue;
      }
      snapshot->portability.push_back(entry.second);
      if (!account(1)) {
        compatible = false;
        snapshot->truncated = true;
      }
    }
    for (const auto& entry : publishers) {
      PublisherStatus status;
      status.id = entry.second.publisher;
      status.boot = entry.second.boot;
      status.coordinator_epoch = entry.second.epoch;
      status.federation = entry.second.federation;
      status.watermark = entry.second.watermark;
      status.registered_at = entry.second.registered_at;
      status.last_publication_at = entry.second.last_publication_at;
      status.publications_accepted = entry.second.accepted;
      status.publications_rejected = entry.second.rejected;
      status.duplicates_suppressed = entry.second.duplicates;
      status.sequence_gaps = entry.second.gaps;
      status.live = entry.second.live;
      status.fenced = entry.second.fenced;
      status.fenced_reason = entry.second.fenced_reason;
      status.evidence_class = entry.second.evidence_class;
      snapshot->publishers.push_back(std::move(status));
    }

    // Every collection is put into identifier order so that the snapshot accessors can
    // binary-search it. This is the contract find_by_id relies on.
    const auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(snapshot->federations.begin(), snapshot->federations.end(), by_id);
    std::sort(snapshot->sites.begin(), snapshot->sites.end(), by_id);
    std::sort(snapshot->clusters.begin(), snapshot->clusters.end(), by_id);
    std::sort(snapshot->accelerator_classes.begin(), snapshot->accelerator_classes.end(), by_id);
    std::sort(snapshot->runtimes.begin(), snapshot->runtimes.end(), by_id);
    std::sort(snapshot->backends.begin(), snapshot->backends.end(), by_id);
    std::sort(snapshot->domains.begin(), snapshot->domains.end(), by_id);
    std::sort(snapshot->policies.begin(), snapshot->policies.end(), by_id);
    std::sort(snapshot->artifacts.begin(), snapshot->artifacts.end(), by_id);
    std::sort(snapshot->workload_classes.begin(), snapshot->workload_classes.end(), by_id);
    std::sort(snapshot->workloads.begin(), snapshot->workloads.end(), by_id);
    std::sort(snapshot->placements.begin(), snapshot->placements.end(), by_id);
    std::sort(snapshot->migrations.begin(), snapshot->migrations.end(), by_id);

    snapshot->records_included = snapshot->federations.size() + snapshot->sites.size() +
                                 snapshot->clusters.size() + snapshot->accelerator_classes.size() +
                                 snapshot->runtimes.size() + snapshot->backends.size() +
                                 snapshot->domains.size() + snapshot->policies.size() +
                                 snapshot->artifacts.size() + snapshot->workload_classes.size() +
                                 snapshot->workloads.size() + snapshot->placements.size() +
                                 snapshot->migrations.size() + snapshot->portability.size();

    if (all && federations.size() == 1) {
      snapshot->federation = federations.begin()->first;
      snapshot->generation = federations.begin()->second.generation;
      snapshot->precision = federations.begin()->second.stamp.precision;
      snapshot->evidence_class = federations.begin()->second.stamp.evidence_class;
    } else if (!all) {
      const auto it = federations.find(restrict_to);
      if (it != federations.end()) {
        snapshot->federation = it->first;
        snapshot->generation = it->second.generation;
        snapshot->precision = it->second.stamp.precision;
        snapshot->evidence_class = it->second.stamp.evidence_class;
      }
    } else {
      snapshot->precision = Precision::Derived;
      snapshot->evidence_class = EvidenceClass::Unknown;
    }
    if (!snapshot->clusters.empty()) {
      EvidenceClass weakest_class = snapshot->clusters.front().stamp.evidence_class;
      Precision weakest_precision = snapshot->clusters.front().stamp.precision;
      for (const ClusterRecord& cluster : snapshot->clusters) {
        weakest_class = weaker(weakest_class, cluster.stamp.evidence_class);
        weakest_precision = weakest(weakest_precision, cluster.stamp.precision);
      }
      if (restrict_to.empty()) {
        snapshot->precision = weakest(snapshot->precision, weakest_precision);
        snapshot->evidence_class = weaker(snapshot->evidence_class, weakest_class);
      }
    }

    SnapshotHealth& health = snapshot->health;
    health.clusters_total = snapshot->clusters.size();
    health.sites_total = snapshot->sites.size();
    for (const ClusterRecord& cluster : snapshot->clusters) {
      switch (cluster.currentness) {
        case Currentness::Current: health.clusters_current += 1; break;
        case Currentness::Stale: health.clusters_stale += 1; break;
        case Currentness::RevalidationRequired: health.clusters_revalidation_required += 1; break;
        case Currentness::Retired: health.clusters_retired += 1; break;
        case Currentness::Unknown: health.clusters_unknown += 1; break;
      }
    }
    for (const SiteRecord& site : snapshot->sites) {
      if (site.currentness == Currentness::Current) {
        health.sites_current += 1;
      }
    }
    health.publishers_total = snapshot->publishers.size();
    for (const PublisherStatus& status : snapshot->publishers) {
      if (status.live) {
        health.publishers_live += 1;
      }
      if (status.fenced) {
        health.publishers_fenced += 1;
      }
    }
    health.revalidation_pending = health.clusters_revalidation_required > 0;
    health.degraded = health.clusters_current != health.clusters_total || health.revalidation_pending;

    if (snapshot->truncated) {
      const Status note = snapshot->evidence.add(
          Provenance::DerivedAnalysis, snapshot->evidence_class, "snapshot", Precision::Derived,
          "snapshot truncated at the configured snapshot byte bound; the federation is NOT fully "
          "represented");
      (void)note;
    }
    return snapshot;
  }

  // ------------------------------------------------------------------
  // Durable state
  // ------------------------------------------------------------------

  [[nodiscard]] DurableState capture_durable_state() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    DurableState state;
    state.format_version = kStateFormatVersion;
    state.written_by_epoch = coordinator_epoch_;
    state.save_sequence = save_sequence + 1;
    state.saved_at = now_unix_nanos();
    for (const auto& entry : federations) state.federations.push_back(entry.second);
    for (const auto& entry : sites) state.sites.push_back(entry.second);
    for (const auto& entry : clusters) {
      ClusterRecord record = entry.second;
      record.capacity_pools.clear();
      record.readiness = Readiness::Unknown;
      if (record.currentness != Currentness::Retired) {
        record.currentness = Currentness::RevalidationRequired;
      }
      state.clusters.push_back(std::move(record));
    }
    for (const auto& entry : accelerator_classes) state.accelerator_classes.push_back(entry.second);
    for (const auto& entry : runtimes) state.runtimes.push_back(entry.second);
    for (const auto& entry : backends) state.backends.push_back(entry.second);
    for (const auto& entry : domains) state.domains.push_back(entry.second);
    for (const auto& entry : policies) state.policies.push_back(entry.second);
    for (const auto& entry : artifacts) state.artifacts.push_back(entry.second);
    for (const auto& entry : workload_classes) state.workload_classes.push_back(entry.second);
    for (const auto& entry : workloads) state.workloads.push_back(entry.second);
    for (const auto& entry : placements) state.placements.push_back(entry.second);
    for (const auto& entry : migrations) state.migrations.push_back(entry.second);
    for (const auto& entry : portability) state.portability.push_back(entry.second);
    for (const auto& entry : publishers) {
      PublisherWatermark watermark;
      watermark.publisher = entry.second.publisher;
      watermark.boot = entry.second.boot;
      watermark.watermark = entry.second.watermark;
      watermark.fenced = entry.second.fenced;
      watermark.fenced_reason = entry.second.fenced_reason;
      watermark.last_seen = entry.second.last_publication_at;
      state.publisher_watermarks.push_back(std::move(watermark));
    }
    state.aggregate_findings = aggregate_findings;
    return state;
  }

  Status apply_durable_state(const DurableState& state) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    const Status valid = state.validate(bounds);
    if (!valid.ok()) {
      return valid.error();
    }

    // Build the whole replacement model first. Nothing is swapped in until every record
    // has been accepted, so a rejected load cannot leave the runtime half-updated.
    std::map<FederationId, FederationRecord> new_federations;
    std::map<SiteId, SiteRecord> new_sites;
    std::map<ClusterId, ClusterRecord> new_clusters;
    std::map<AcceleratorClassId, AcceleratorClassRecord> new_classes;
    std::map<RuntimeId, RuntimeRecord> new_runtimes;
    std::map<BackendId, BackendRecord> new_backends;
    std::map<DomainId, DomainRecord> new_domains;
    std::map<PolicyId, PolicyRecord> new_policies;
    std::map<ArtifactId, ArtifactRecord> new_artifacts;
    std::map<WorkloadClassId, WorkloadClassRecord> new_workload_classes;
    std::map<WorkloadId, WorkloadRecord> new_workloads;
    std::map<PlacementId, PlacementRecord> new_placements;
    std::map<WorkloadId, PlacementId> new_latest_placement;
    std::map<MigrationId, MigrationRecord> new_migrations;
    std::map<WorkloadId, MigrationId> new_latest_migration;
    std::map<WorkloadId, MigrationId> new_superseded;
    std::map<PortabilityKey, PortabilityAssessment> new_portability;
    std::map<PublisherKey, PublisherAuthority> new_publishers;

    for (const FederationRecord& record : state.federations) {
      FederationRecord copy = record;
      if (copy.currentness == Currentness::Current) {
        copy.currentness = Currentness::Stale;
      }
      new_federations.emplace(copy.id, std::move(copy));
    }
    for (const SiteRecord& record : state.sites) {
      SiteRecord copy = record;
      if (copy.currentness == Currentness::Current) {
        copy.currentness = Currentness::Stale;
      }
      new_sites.emplace(copy.id, std::move(copy));
    }
    for (const ClusterRecord& record : state.clusters) {
      ClusterRecord copy = record;
      copy.capacity_pools.clear();
      copy.readiness = Readiness::Unknown;
      if (copy.currentness != Currentness::Retired) {
        copy.currentness = Currentness::RevalidationRequired;
      }
      new_clusters.emplace(copy.id, std::move(copy));
    }
    for (const AcceleratorClassRecord& record : state.accelerator_classes) {
      AcceleratorClassRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_classes.emplace(copy.id, std::move(copy));
    }
    for (const RuntimeRecord& record : state.runtimes) {
      RuntimeRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_runtimes.emplace(copy.id, std::move(copy));
    }
    for (const BackendRecord& record : state.backends) {
      BackendRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_backends.emplace(copy.id, std::move(copy));
    }
    for (const DomainRecord& record : state.domains) {
      DomainRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_domains.emplace(copy.id, std::move(copy));
    }
    for (const PolicyRecord& record : state.policies) {
      PolicyRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_policies.emplace(copy.id, std::move(copy));
    }
    for (const ArtifactRecord& record : state.artifacts) {
      ArtifactRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_artifacts.emplace(copy.id, std::move(copy));
    }
    for (const WorkloadClassRecord& record : state.workload_classes) {
      WorkloadClassRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_workload_classes.emplace(copy.id, std::move(copy));
    }
    for (const WorkloadRecord& record : state.workloads) {
      WorkloadRecord copy = record;
      if (copy.currentness == Currentness::Current) copy.currentness = Currentness::Stale;
      new_workloads.emplace(copy.id, std::move(copy));
    }
    for (const PlacementRecord& record : state.placements) {
      PlacementRecord copy = record;
      copy.currentness = Currentness::Stale;
      const PlacementId id = copy.id;
      const WorkloadId workload = copy.workload;
      auto it = new_latest_placement.find(workload);
      if (it == new_latest_placement.end() ||
          new_placements[it->second].stamp.observed_at <= copy.stamp.observed_at) {
        new_latest_placement[workload] = id;
      }
      new_placements.emplace(id, std::move(copy));
    }
    for (const MigrationRecord& record : state.migrations) {
      MigrationRecord copy = record;
      copy.currentness = Currentness::Stale;
      const MigrationId id = copy.id;
      const WorkloadId workload = copy.workload;
      if (!copy.supersedes.empty()) {
        new_superseded[workload] = copy.supersedes;
      }
      auto it = new_latest_migration.find(workload);
      if (it == new_latest_migration.end()) {
        new_latest_migration[workload] = id;
      }
      new_migrations.emplace(id, std::move(copy));
    }
    for (const PortabilityAssessment& record : state.portability) {
      PortabilityAssessment copy = record;
      copy.currentness = Currentness::Stale;
      new_portability.emplace(PortabilityKey{copy.workload, copy.destination}, std::move(copy));
    }
    for (const PublisherWatermark& watermark : state.publisher_watermarks) {
      PublisherAuthority authority;
      authority.publisher = watermark.publisher;
      authority.boot = watermark.boot;
      authority.epoch = state.written_by_epoch;
      authority.watermark = watermark.watermark;
      authority.fenced = watermark.fenced;
      authority.fenced_reason = watermark.fenced_reason;
      authority.last_publication_at = watermark.last_seen;
      authority.live = false;
      new_publishers.emplace(PublisherKey{watermark.publisher, watermark.boot}, std::move(authority));
    }

    federations = std::move(new_federations);
    sites = std::move(new_sites);
    clusters = std::move(new_clusters);
    accelerator_classes = std::move(new_classes);
    runtimes = std::move(new_runtimes);
    backends = std::move(new_backends);
    domains = std::move(new_domains);
    policies = std::move(new_policies);
    artifacts = std::move(new_artifacts);
    workload_classes = std::move(new_workload_classes);
    workloads = std::move(new_workloads);
    placements = std::move(new_placements);
    latest_placement = std::move(new_latest_placement);
    migrations = std::move(new_migrations);
    latest_migration = std::move(new_latest_migration);
    superseded_migrations = std::move(new_superseded);
    portability = std::move(new_portability);
    publishers = std::move(new_publishers);
    aggregate_findings = state.aggregate_findings;
    save_sequence = state.save_sequence;

    // Advance the coordinator epoch exactly once per restore. Every frame stamped with
    // the previous epoch is now permanently refused.
    coordinator_epoch_ = state.written_by_epoch.is_set() ? state.written_by_epoch.next()
                                                         : CoordinatorEpoch{1};
    snapshot_generation_ = snapshot_generation_.next();
    stats.state_loads += 1;
    return Status::success();
  }

  Status invalidate_dynamic_evidence(const std::string& reason) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    for (auto& entry : clusters) {
      ClusterRecord& cluster = entry.second;
      cluster.capacity_pools.clear();
      cluster.readiness = Readiness::Unknown;
      if (cluster.currentness != Currentness::Retired) {
        cluster.currentness = Currentness::RevalidationRequired;
      }
      const Status note = cluster.evidence.add(
          Provenance::DerivedAnalysis, EvidenceClass::Unknown, "observatory", Precision::Derived,
          "dynamic evidence invalidated: " + reason);
      (void)note;
    }
    for (auto& entry : publishers) {
      if (!entry.second.fenced) {
        entry.second.live = false;
      }
    }
    snapshot_generation_ = snapshot_generation_.next();
    return Status::success();
  }
};

// ---------------------------------------------------------------------------
// FederationObservatory
// ---------------------------------------------------------------------------

FederationObservatory::FederationObservatory(ObservatoryConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

FederationObservatory::~FederationObservatory() = default;

Result<IngestResult> FederationObservatory::register_publisher(const PublicationContext& ctx) {
  return impl_->register_publisher(ctx);
}

Status FederationObservatory::fence_publisher(const PublisherId& publisher, BootGeneration boot,
                                              std::string reason) {
  return impl_->fence_publisher(publisher, boot, std::move(reason));
}

bool FederationObservatory::publisher_is_live(const PublisherId& publisher,
                                              BootGeneration boot) const {
  return impl_->publisher_is_live(publisher, boot);
}

Result<IngestResult> FederationObservatory::register_federation(const PublicationContext& ctx,
                                                                FederationRecord record) {
  return impl_->register_federation(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_site(const PublicationContext& ctx,
                                                          SiteRecord record) {
  return impl_->register_site(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_cluster(const PublicationContext& ctx,
                                                             ClusterRecord record) {
  return impl_->register_cluster(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_accelerator_class(
    const PublicationContext& ctx, AcceleratorClassRecord record) {
  return impl_->register_accelerator_class(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_runtime(const PublicationContext& ctx,
                                                             RuntimeRecord record) {
  return impl_->register_runtime(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_backend(const PublicationContext& ctx,
                                                             BackendRecord record) {
  return impl_->register_backend(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_domain(const PublicationContext& ctx,
                                                            DomainRecord record) {
  return impl_->register_domain(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_policy(const PublicationContext& ctx,
                                                            PolicyRecord record) {
  return impl_->register_policy(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_artifact(const PublicationContext& ctx,
                                                              ArtifactRecord record) {
  return impl_->register_artifact(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_workload_class(const PublicationContext& ctx,
                                                                    WorkloadClassRecord record) {
  return impl_->register_workload_class(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::register_workload(const PublicationContext& ctx,
                                                              WorkloadRecord record) {
  return impl_->register_workload(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::retire_cluster(const PublicationContext& ctx,
                                                           const ClusterId& cluster,
                                                           ClusterGeneration generation,
                                                           std::string reason) {
  return impl_->retire_cluster(ctx, cluster, generation, std::move(reason));
}

Result<IngestResult> FederationObservatory::publish_capability(
    const PublicationContext& ctx, const ClusterId& cluster, ClusterGeneration cluster_generation,
    AcceleratorCapabilityGeneration capability_generation, CapabilitySet capabilities) {
  return impl_->publish_capability(ctx, cluster, cluster_generation, capability_generation,
                                   std::move(capabilities));
}

Result<IngestResult> FederationObservatory::publish_capacity(const PublicationContext& ctx,
                                                             const ClusterId& cluster,
                                                             ClusterGeneration cluster_generation,
                                                             CapacityGeneration capacity_generation,
                                                             std::vector<CapacityPool> pools) {
  return impl_->publish_capacity(ctx, cluster, cluster_generation, capacity_generation,
                                 std::move(pools));
}

Result<IngestResult> FederationObservatory::publish_placement(const PublicationContext& ctx,
                                                              PlacementRecord record) {
  return impl_->publish_placement(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::publish_migration(const PublicationContext& ctx,
                                                              MigrationRecord record) {
  return impl_->publish_migration(ctx, std::move(record));
}

Result<IngestResult> FederationObservatory::publish_migration_stage(
    const PublicationContext& ctx, const MigrationId& migration, MigrationGeneration generation,
    MigrationStage stage, std::string detail) {
  return impl_->publish_migration_stage(ctx, migration, generation, stage, std::move(detail));
}

Result<IngestResult> FederationObservatory::publish_portability(const PublicationContext& ctx,
                                                                PortabilityAssessment record) {
  return impl_->publish_portability(ctx, std::move(record));
}

SnapshotHandle FederationObservatory::snapshot() const { return impl_->build_snapshot(FederationId{}); }

SnapshotHandle FederationObservatory::snapshot_for(const FederationId& federation) const {
  return impl_->build_snapshot(federation);
}

namespace {

/// Forward declaration: the definition follows with the rest of the assessment helpers.
void collect_assessment_inputs(const FederationSnapshot& snapshot, const ClusterId& cluster_id,
                               const WorkloadId& workload_id, CompatibilityInputs& inputs,
                               CapabilitySet& effective, PolicyRecord& policy_storage,
                               bool& policy_present);

}  // namespace

Result<PlacementExplanation> FederationObservatory::explain_placement(
    const PlacementId& placement) const {
  // One snapshot serves the whole explanation. Taking a fresh snapshot per candidate would
  // make an explanation cost a full federation copy per candidate.
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const PlacementRecord* record = handle->find_placement(placement);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "placement is not registered", placement.value());
  }
  PlacementExplanationInputs inputs;
  inputs.placement = record;
  inputs.snapshot_generation = handle->snapshot_generation;

  CapabilitySet effective;
  PolicyRecord policy_storage;
  bool policy_present = false;
  const auto assess = [&](const ClusterId& cluster) {
    if (handle->find_cluster(cluster) == nullptr) {
      return;
    }
    CompatibilityInputs compatibility_inputs;
    collect_assessment_inputs(*handle, cluster, record->workload, compatibility_inputs, effective,
                              policy_storage, policy_present);
    inputs.assessments.push_back(assess_compatibility(compatibility_inputs));
  };
  assess(record->selected);
  for (const CandidateObservation& candidate : record->candidates) {
    if (candidate.status == CandidateStatus::Selected && candidate.cluster != record->selected) {
      assess(candidate.cluster);
    }
  }
  return fo::explain_placement(inputs);
}

Result<PlacementExplanation> FederationObservatory::explain_latest_placement(
    const WorkloadId& workload) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const PlacementRecord* latest = nullptr;
  for (const PlacementRecord& record : handle->placements) {
    if (record.workload != workload) {
      continue;
    }
    if (record.currentness != Currentness::Current) {
      continue;
    }
    if (latest == nullptr || latest->stamp.observed_at <= record.stamp.observed_at) {
      latest = &record;
    }
  }
  if (latest == nullptr) {
    return Error(ErrorCode::NotFound, "no current placement is registered for this workload",
                 workload.value());
  }
  return explain_placement(latest->id);
}

Result<RejectionExplanation> FederationObservatory::explain_rejection(
    const PlacementId& placement, const ClusterId& candidate) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const PlacementRecord* record = handle->find_placement(placement);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "placement is not registered", placement.value());
  }
  return build_rejection_explanation(*record, candidate);
}

Result<StrandedCapacityReport> FederationObservatory::stranded_capacity(
    const StrandedCapacityRequest& request) const {
  const SnapshotHandle handle = impl_->build_snapshot(request.federation);
  return analyze_stranded_capacity(*handle, request);
}

Result<FragmentationFinding> FederationObservatory::fragmentation(
    const FederationId& federation, const WorkloadClassId& workload_class,
    ResourceKind kind) const {
  const SnapshotHandle handle = impl_->build_snapshot(federation);
  return analyze_fragmentation(*handle, federation, workload_class, kind);
}

namespace {

void collect_assessment_inputs(const FederationSnapshot& snapshot, const ClusterId& cluster_id,
                               const WorkloadId& workload_id, CompatibilityInputs& inputs,
                               CapabilitySet& effective, PolicyRecord& policy_storage,
                               bool& policy_present) {
  const ClusterRecord* cluster = snapshot.find_cluster(cluster_id);
  inputs.cluster = cluster;
  if (cluster == nullptr) {
    return;
  }
  inputs.target_currentness = cluster->currentness;
  const WorkloadRecord* workload = snapshot.find_workload(workload_id);
  inputs.workload = workload;
  if (workload != nullptr) {
    inputs.artifact = snapshot.find_artifact(workload->artifact);
    if (!workload->policy.empty()) {
      const PolicyRecord* policy = snapshot.find_policy(workload->policy);
      if (policy != nullptr) {
        policy_storage = *policy;
        policy_present = true;
        inputs.policy = &policy_storage;
      }
    }
  }
  const AcceleratorClassRecord* accelerator_class = nullptr;
  if (workload != nullptr && !workload->acceptable_accelerator_classes.empty()) {
    accelerator_class = snapshot.find_accelerator_class(workload->acceptable_accelerator_classes.front());
  }
  if (accelerator_class == nullptr) {
    for (const AcceleratorClassId& id : cluster->accelerator_classes) {
      accelerator_class = snapshot.find_accelerator_class(id);
      if (accelerator_class != nullptr) {
        break;
      }
    }
  }
  inputs.accelerator_class = accelerator_class;
  if (!cluster->runtimes.empty()) {
    inputs.cluster_runtime = snapshot.find_runtime(cluster->runtimes.front());
  }
  // Same precedence as the analyser: accelerator class, then cluster, then runtime.
  if (accelerator_class != nullptr) {
    for (const CapabilityEntry& entry : accelerator_class->capabilities.entries()) {
      const Status s = effective.put(entry);
      (void)s;
    }
  }
  for (const CapabilityEntry& entry : cluster->capabilities.entries()) {
    const Status s = effective.put(entry);
    (void)s;
  }
  if (inputs.cluster_runtime != nullptr) {
    for (const CapabilityEntry& entry : inputs.cluster_runtime->capabilities.entries()) {
      const Status s = effective.put(entry);
      (void)s;
    }
  }
  inputs.effective_capabilities = &effective;
  inputs.compatibility_generation = cluster->compatibility_generation;
}

}  // namespace

Result<CompatibilityAssessment> FederationObservatory::compatibility(const ClusterId& cluster,
                                                                     const WorkloadId& workload) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  if (handle->find_cluster(cluster) == nullptr) {
    return Error(ErrorCode::NotFound, "cluster is not registered", cluster.value());
  }
  if (handle->find_workload(workload) == nullptr) {
    return Error(ErrorCode::NotFound, "workload is not registered", workload.value());
  }
  CompatibilityInputs inputs;
  CapabilitySet effective;
  PolicyRecord policy_storage;
  bool policy_present = false;
  collect_assessment_inputs(*handle, cluster, workload, inputs, effective, policy_storage,
                            policy_present);
  return assess_compatibility(inputs);
}

namespace {

PortabilityInputs portability_inputs_for(const FederationSnapshot& snapshot,
                                         const WorkloadId& workload_id,
                                         const ClusterId& destination_id,
                                         PolicyRecord& policy_storage, bool& policy_present,
                                         CapabilitySet& effective,
                                         AcceleratorClassRecord& class_storage,
                                         bool& class_present, ClusterRecord& source_storage,
                                         bool& source_present, RuntimeRecord& runtime_storage,
                                         bool& runtime_present) {
  PortabilityInputs inputs;
  const WorkloadRecord* workload = snapshot.find_workload(workload_id);
  inputs.workload = workload;
  if (workload != nullptr) {
    inputs.artifact = snapshot.find_artifact(workload->artifact);
    if (!workload->policy.empty()) {
      const PolicyRecord* policy = snapshot.find_policy(workload->policy);
      if (policy != nullptr) {
        policy_storage = *policy;
        policy_present = true;
        inputs.policy = &policy_storage;
      }
    }
  }

  // Source: the cluster that currently hosts the workload, taken from the latest
  // current placement when one exists.
  const PlacementRecord* latest = nullptr;
  for (const PlacementRecord& placement : snapshot.placements) {
    if (placement.workload != workload_id || placement.currentness != Currentness::Current) {
      continue;
    }
    if (latest == nullptr || latest->stamp.observed_at <= placement.stamp.observed_at) {
      latest = &placement;
    }
  }
  if (latest != nullptr) {
    if (const ClusterRecord* cluster = snapshot.find_cluster(latest->selected)) {
      source_storage = *cluster;
      source_present = true;
      inputs.source_cluster = &source_storage;
      inputs.source_runtime = nullptr;
      if (!cluster->runtimes.empty()) {
        if (const RuntimeRecord* runtime = snapshot.find_runtime(cluster->runtimes.front())) {
          runtime_storage = *runtime;
          runtime_present = true;
          inputs.source_runtime = &runtime_storage;
        }
      }
      for (const AcceleratorClassId& id : cluster->accelerator_classes) {
        if (const AcceleratorClassRecord* accelerator_class = snapshot.find_accelerator_class(id)) {
          inputs.source_accelerator_class = accelerator_class;
          break;
        }
      }
    }
  }

  const ClusterRecord* destination = snapshot.find_cluster(destination_id);
  inputs.destination_cluster = destination;
  if (destination != nullptr) {
    if (!destination->runtimes.empty()) {
      inputs.destination_runtime = snapshot.find_runtime(destination->runtimes.front());
    }
    const AcceleratorClassRecord* accelerator_class = nullptr;
    if (workload != nullptr && !workload->acceptable_accelerator_classes.empty()) {
      accelerator_class =
          snapshot.find_accelerator_class(workload->acceptable_accelerator_classes.front());
    }
    if (accelerator_class == nullptr) {
      for (const AcceleratorClassId& id : destination->accelerator_classes) {
        accelerator_class = snapshot.find_accelerator_class(id);
        if (accelerator_class != nullptr) {
          break;
        }
      }
    }
    if (accelerator_class != nullptr) {
      class_storage = *accelerator_class;
      class_present = true;
      inputs.destination_accelerator_class = &class_storage;
    }
    if (inputs.destination_accelerator_class != nullptr) {
      for (const CapabilityEntry& entry : inputs.destination_accelerator_class->capabilities.entries()) {
        const Status s = effective.put(entry);
        (void)s;
      }
    }
    for (const CapabilityEntry& entry : destination->capabilities.entries()) {
      const Status s = effective.put(entry);
      (void)s;
    }
    if (inputs.destination_runtime != nullptr) {
      for (const CapabilityEntry& entry : inputs.destination_runtime->capabilities.entries()) {
        const Status s = effective.put(entry);
        (void)s;
      }
    }
    inputs.destination_capabilities = &effective;
    inputs.federation = destination->federation;
    inputs.federation_generation = snapshot.generation;
  }
  return inputs;
}

}  // namespace

Result<PortabilityAssessment> FederationObservatory::portability(const WorkloadId& workload,
                                                                 const ClusterId& destination) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const WorkloadRecord* record = handle->find_workload(workload);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "workload is not registered", workload.value());
  }
  for (const PortabilityAssessment& assessment : handle->portability) {
    if (assessment.workload == workload && assessment.destination == destination) {
      return assessment;
    }
  }
  return Error(ErrorCode::NotFound, "no portability assessment has been published for this pair",
               workload.value() + " -> " + destination.value());
}

Result<PortabilityAssessment> FederationObservatory::evaluate_portability(
    const WorkloadId& workload, const ClusterId& destination) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  if (handle->find_workload(workload) == nullptr) {
    return Error(ErrorCode::NotFound, "workload is not registered", workload.value());
  }
  if (handle->find_cluster(destination) == nullptr) {
    return Error(ErrorCode::NotFound, "destination cluster is not registered", destination.value());
  }
  PolicyRecord policy_storage;
  bool policy_present = false;
  CapabilitySet effective;
  AcceleratorClassRecord class_storage;
  bool class_present = false;
  ClusterRecord source_storage;
  bool source_present = false;
  RuntimeRecord runtime_storage;
  bool runtime_present = false;
  const PortabilityInputs inputs =
      portability_inputs_for(*handle, workload, destination, policy_storage, policy_present,
                             effective, class_storage, class_present, source_storage, source_present,
                             runtime_storage, runtime_present);
  return assess_portability(inputs);
}

Result<MigrationAnalysis> FederationObservatory::migration_analysis(const MigrationId& migration) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const MigrationRecord* record = handle->find_migration(migration);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "migration is not registered", migration.value());
  }
  MigrationAnalysisInputs inputs;
  inputs.migration = record;
  inputs.snapshot_generation = handle->snapshot_generation;
  for (const PortabilityAssessment& assessment : handle->portability) {
    if (assessment.workload == record->workload && assessment.destination == record->destination &&
        assessment.source == record->source) {
      inputs.portability = &assessment;
      break;
    }
  }
  PortabilityAssessment computed_storage;
  if (inputs.portability == nullptr) {
    const Result<PortabilityAssessment> computed =
        evaluate_portability(record->workload, record->destination);
    if (computed.ok()) {
      computed_storage = computed.value();
      inputs.portability = &computed_storage;
    }
  }
  for (const MigrationRecord& candidate : handle->migrations) {
    if (candidate.workload == record->workload && candidate.supersedes == record->id) {
      inputs.superseded_by = candidate.id;
      break;
    }
  }
  return analyze_migration(inputs);
}

Result<MismatchAnalysis> FederationObservatory::mismatch_analysis(
    const MismatchAnalysisRequest& request) const {
  const SnapshotHandle handle = impl_->build_snapshot(request.federation);
  return analyze_mismatch(*handle, request);
}

Result<DriftReport> FederationObservatory::drift(const DriftRequest& request) const {
  const SnapshotHandle handle = impl_->build_snapshot(request.federation);
  return analyze_drift(*handle, request);
}

Result<FederationRecord> FederationObservatory::federation(const FederationId& id) const {
  const SnapshotHandle handle = impl_->build_snapshot(FederationId{});
  const FederationRecord* record = handle->find_federation(id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "federation is not registered", id.value());
  }
  return *record;
}

Status FederationObservatory::save_state(const std::string& path) const {
  const DurableState state = impl_->capture_durable_state();
  {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->save_sequence += 1;
  }
  const Status saved = save_durable_state(state, path, impl_->bounds);
  if (saved.ok()) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->stats.state_saves += 1;
  }
  return saved;
}

Status FederationObservatory::checkpoint(const std::string& path) const { return save_state(path); }

Status FederationObservatory::load_state(const std::string& path) {
  const Result<DurableState> state = load_durable_state(path, impl_->bounds);
  if (!state.ok()) {
    return state.error();
  }
  return impl_->apply_durable_state(state.value());
}

Status FederationObservatory::restore(const DurableState& state) {
  return impl_->apply_durable_state(state);
}

const Bounds& FederationObservatory::bounds() const noexcept { return impl_->bounds; }

CoordinatorEpoch FederationObservatory::coordinator_epoch() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->coordinator_epoch_;
}

SnapshotGeneration FederationObservatory::snapshot_generation() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->snapshot_generation_;
}

ObservatoryStats FederationObservatory::stats() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->stats;
}

Status FederationObservatory::invalidate_dynamic_evidence(std::string reason) {
  return impl_->invalidate_dynamic_evidence(reason);
}

}  // namespace fo
