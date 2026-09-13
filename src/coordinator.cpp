// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// The coordinator: one operating-system process that owns the authoritative observation
// state. Connection threads read frames and hand them to a bounded ingest queue; ingest
// workers apply them under the observatory's own lock. No lock is ever held across
// socket I/O, and shutdown releases every thread blocked on a queue or a socket instead
// of terminating a blocked system call.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
// std::fopen is used deliberately for the append-only operational log.
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "federation_observatory/coordinator.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/net.hpp"
#include "federation_observatory/process.hpp"
#include "federation_observatory/version.hpp"

namespace fo {
namespace {

protocol::Frame make_reply(const protocol::Frame& request, protocol::ReplyPayload payload) {
  protocol::Frame reply;
  reply.type = payload.code == ErrorCode::Ok ? protocol::MessageType::Reply
                                             : protocol::MessageType::Failure;
  reply.flags = protocol::frame_flag(protocol::FrameFlags::Reply);
  reply.request_id = request.request_id;
  Encoder encoder;
  protocol::encode(encoder, payload);
  if (encoder.failed()) {
    protocol::ReplyPayload failure;
    failure.code = ErrorCode::Internal;
    failure.message = "the coordinator could not encode its reply";
    Encoder fallback;
    protocol::encode(fallback, failure);
    reply.type = protocol::MessageType::Failure;
    reply.payload = fallback.take();
    return reply;
  }
  reply.payload = encoder.take();
  return reply;
}

protocol::Frame make_error_reply(const protocol::Frame& request, ErrorCode code, std::string message,
                                std::string detail = {}) {
  protocol::ReplyPayload payload;
  payload.code = code;
  payload.message = std::move(message);
  payload.detail = std::move(detail);
  return make_reply(request, std::move(payload));
}

protocol::Frame make_publication_reply(const protocol::Frame& request,
                                       const Result<IngestResult>& result) {
  if (!result.ok()) {
    return make_error_reply(request, result.error().code(), result.error().message(),
                            result.error().detail());
  }
  protocol::Frame reply;
  reply.type = protocol::MessageType::Reply;
  reply.flags = protocol::frame_flag(protocol::FrameFlags::Reply);
  reply.request_id = request.request_id;
  protocol::PublicationAck ack;
  ack.disposition = result.value().disposition;
  ack.code = result.value().code;
  ack.detail = result.value().detail;
  ack.watermark = result.value().watermark;
  ack.sequence_gap = result.value().sequence_gap;
  Encoder encoder;
  protocol::encode(encoder, ack);
  reply.payload = encoder.take();
  return reply;
}

protocol::WireRow wire_row(std::initializer_list<std::pair<const char*, std::string>> fields) {
  protocol::WireRow row;
  for (const auto& field : fields) {
    protocol::WireField element;
    element.name = field.first;
    element.value = field.second;
    row.fields.push_back(std::move(element));
  }
  return row;
}

}  // namespace

Status CoordinatorConfig::validate() const {
  if (bind_host.empty()) {
    return fail(ErrorCode::InvalidArgument, "coordinator bind host is empty");
  }
  const Status bounds_status = bounds.validate();
  if (!bounds_status.ok()) {
    return bounds_status.error();
  }
  if (worker_threads == 0 || worker_threads > 64) {
    return fail(ErrorCode::InvalidArgument, "worker thread count must be between 1 and 64");
  }
  if (accept_threads == 0 || accept_threads > 8) {
    return fail(ErrorCode::InvalidArgument, "accept thread count must be between 1 and 8");
  }
  if (poll_millis < 1 || poll_millis > 5000) {
    return fail(ErrorCode::InvalidArgument, "poll quantum must be between 1 and 5000 milliseconds");
  }
  if (max_connections == 0 || max_connections > bounds.max_connections) {
    return fail(ErrorCode::InvalidArgument,
                "max_connections must be between 1 and the configured connection bound");
  }
  return Status::success();
}

class FederationCoordinator::Impl {
 public:
  struct WorkItem {
    protocol::Frame frame;
    std::string peer;
    std::promise<Result<protocol::Frame>> completion;
  };

  explicit Impl(CoordinatorConfig config_in) : config(std::move(config_in)) {
    ObservatoryConfig observatory_config;
    observatory_config.bounds = config.bounds;
    observatory = std::make_unique<FederationObservatory>(observatory_config);
  }

  ~Impl() {
    if (log_file != nullptr) {
      std::fclose(log_file);
      log_file = nullptr;
    }
  }

  CoordinatorConfig config;
  std::unique_ptr<FederationObservatory> observatory;
  net::TcpListener listener;
  std::string coordinator_id;

  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  /// Set by a SHUTDOWN frame. A closed control channel is NOT a stop request: a
  /// coordinator with no console must keep serving, not exit.
  std::atomic<bool> shutdown_requested{false};

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<std::shared_ptr<WorkItem>> queue;

  std::mutex connections_mutex;
  std::map<std::uint64_t, std::shared_ptr<net::Socket>> connections;
  std::uint64_t next_connection_id = 0;
  std::vector<std::thread> connection_threads;
  std::vector<std::thread> accept_threads;
  std::vector<std::thread> ingest_threads;

  mutable std::mutex counters_mutex;
  Counters counters;

  std::mutex log_mutex;
  std::FILE* log_file = nullptr;

  void log_line(const std::string& text) {
    if (config.log_path.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_file == nullptr) {
      log_file = std::fopen(config.log_path.c_str(), "ab");
      if (log_file == nullptr) {
        return;
      }
    }
    const std::string line = text + "\n";
    std::fwrite(line.data(), 1, line.size(), log_file);
    std::fflush(log_file);
  }

  [[nodiscard]] Result<protocol::Frame> dispatch(const protocol::Frame& frame,
                                                 const std::string& peer);
};

namespace {

/// Attach the originating publisher identity to the log line without trusting any
/// payload field that has not already been validated.
std::string frame_label(const protocol::Frame& frame) {
  return std::string(protocol::to_string(frame.type)) + " id=" + std::to_string(frame.request_id);
}

protocol::ReplyPayload reply_from_stranded(const StrandedCapacityReport& report) {
  protocol::ReplyPayload payload;
  payload.code = ErrorCode::Ok;
  payload.message = "stranded capacity report";
  payload.digest = report.digest();
  payload.text = report.render();
  payload.rows.push_back(wire_row({{"workload_class", report.workload_class.value()},
                                   {"nominal", std::to_string(report.summary.nominal)},
                                   {"idle", std::to_string(report.summary.idle)},
                                   {"usable", std::to_string(report.summary.usable)},
                                   {"stranded", std::to_string(report.summary.stranded)},
                                   {"unknown", std::to_string(report.summary.unknown)},
                                   {"closes", report.closes() ? "yes" : "no"},
                                   {"precision", std::string(fo::to_string(report.precision))},
                                   {"evidence_class",
                                    std::string(fo::to_string(report.evidence_class))}}));
  return payload;
}

protocol::ReplyPayload reply_from_fragmentation(const FragmentationFinding& finding) {
  protocol::ReplyPayload payload;
  payload.code = ErrorCode::Ok;
  payload.message = "federation fragmentation";
  payload.digest = finding.digest();
  payload.text = finding.render();
  payload.rows.push_back(
      wire_row({{"classification", std::string(fo::to_string(finding.classification))},
                {"required_per_group", std::to_string(finding.required_per_group)},
                {"aggregate_usable", std::to_string(finding.aggregate_usable)},
                {"aggregate_nominal", std::to_string(finding.aggregate_nominal)},
                {"largest_legal_group", std::to_string(finding.largest_legal_group)},
                {"domains", std::to_string(finding.domains.size())}}));
  return payload;
}

protocol::ReplyPayload reply_from_mismatch(const MismatchAnalysis& analysis) {
  protocol::ReplyPayload payload;
  payload.code = ErrorCode::Ok;
  payload.message = "capability mismatch analysis";
  payload.digest = analysis.digest();
  payload.text = analysis.render();
  for (const MismatchRow& row : analysis.by_capability_key) {
    payload.rows.push_back(wire_row({{"capability", row.subject},
                                     {"population", std::to_string(row.population)},
                                     {"satisfied", std::to_string(row.satisfied)},
                                     {"missing", std::to_string(row.missing)},
                                     {"unknown", std::to_string(row.unknown)}}));
  }
  return payload;
}

protocol::ReplyPayload reply_from_snapshot(const FederationSnapshot& snapshot) {
  protocol::ReplyPayload payload;
  payload.code = ErrorCode::Ok;
  payload.message = "federation snapshot";
  payload.digest = snapshot.digest();
  payload.text = snapshot.render();
  for (const ClusterRecord& cluster : snapshot.clusters) {
    const Result<CapacityLedger> ledger = cluster.total_ledger(ResourceKind::Accelerator);
    payload.rows.push_back(wire_row(
        {{"cluster", cluster.id.value()},
         {"generation", cluster.generation.to_string()},
         {"epoch", cluster.epoch.to_string()},
         {"site", cluster.site.value()},
         {"readiness", std::string(fo::to_string(cluster.readiness))},
         {"currentness", std::string(fo::to_string(cluster.currentness))},
         {"nominal_accelerators",
          ledger.ok() ? std::to_string(ledger.value().nominal) : std::string("UNKNOWN")},
         {"idle_accelerators",
          ledger.ok() ? std::to_string(ledger.value().idle) : std::string("UNKNOWN")},
         {"evidence_class", std::string(fo::to_string(cluster.stamp.evidence_class))}}));
  }
  for (const PublisherStatus& status : snapshot.publishers) {
    payload.rows.push_back(wire_row({{"publisher", status.id.value()},
                                     {"boot", status.boot.to_string()},
                                     {"live", status.live ? "yes" : "no"},
                                     {"fenced", status.fenced ? "yes" : "no"},
                                     {"watermark", status.watermark.to_string()}}));
  }
  return payload;
}

}  // namespace

Result<protocol::Frame> FederationCoordinator::Impl::dispatch(const protocol::Frame& frame,
                                                              const std::string& peer) {
  const bool fence_allowed = config.allow_fence;
  const std::string& admin_token = config.admin_token;
  FederationObservatory& obs = *observatory;

  switch (frame.type) {
    case protocol::MessageType::Hello: {
      Decoder decoder(frame.payload.data(), frame.payload.size(), config.bounds.max_frame_bytes);
      const Result<protocol::HelloRequest> request = protocol::decode_hello(decoder);
      if (!request.ok()) {
        return make_error_reply(frame, request.error().code(), request.error().message(),
                                request.error().detail());
      }
      if (!decoder.exhausted()) {
        return make_error_reply(frame, ErrorCode::ProtocolViolation,
                                "HELLO carries trailing bytes");
      }
      protocol::HelloReply reply;
      reply.protocol = static_cast<std::uint16_t>(protocol::kProtocolVersion);
      reply.server_version = std::string(version_string());
      reply.epoch = obs.coordinator_epoch();
      reply.coordinator_id = coordinator_id;
      protocol::Frame out;
      out.type = protocol::MessageType::HelloAck;
      out.flags = protocol::frame_flag(protocol::FrameFlags::Reply);
      out.request_id = frame.request_id;
      Encoder encoder;
      protocol::encode(encoder, reply);
      out.payload = encoder.take();
      return out;
    }
    case protocol::MessageType::Heartbeat: {
      protocol::Frame out;
      out.type = protocol::MessageType::HeartbeatAck;
      out.flags = protocol::frame_flag(protocol::FrameFlags::Reply);
      out.request_id = frame.request_id;
      return out;
    }
    case protocol::MessageType::RegisterPublisher: {
      Decoder decoder(frame.payload.data(), frame.payload.size(), config.bounds.max_frame_bytes);
      const Result<protocol::RegisterPublisherRequest> request =
          protocol::decode_register_publisher(decoder);
      if (!request.ok()) {
        return make_error_reply(frame, request.error().code(), request.error().message(),
                                request.error().detail());
      }
      return make_publication_reply(frame, obs.register_publisher(request.value().context));
    }
    case protocol::MessageType::FencePublisher: {
      if (!fence_allowed) {
        return make_error_reply(frame, ErrorCode::UnsupportedOperation,
                                "this coordinator does not accept remote fences");
      }
      Decoder decoder(frame.payload.data(), frame.payload.size(), config.bounds.max_frame_bytes);
      const Result<protocol::FenceRequest> request = protocol::decode_fence(decoder);
      if (!request.ok()) {
        return make_error_reply(frame, request.error().code(), request.error().message(),
                                request.error().detail());
      }
      if (!admin_token.empty() && request.value().token != admin_token) {
        return make_error_reply(frame, ErrorCode::UnsupportedOperation,
                                "administrative token does not match");
      }
      const Status fenced =
          obs.fence_publisher(request.value().publisher, request.value().boot, request.value().reason);
      if (!fenced.ok()) {
        return make_error_reply(frame, fenced.error().code(), fenced.error().message(),
                                fenced.error().detail());
      }
      protocol::ReplyPayload payload;
      payload.code = ErrorCode::Ok;
      payload.message = "publisher boot identity fenced";
      payload.text = "fenced publisher " + request.value().publisher.value() + " boot " +
                     request.value().boot.to_string();
      return make_reply(frame, std::move(payload));
    }
    default:
      break;
  }

  if (protocol::is_registration(frame.type) || protocol::is_publication(frame.type)) {
    Decoder decoder(frame.payload.data(), frame.payload.size(), config.bounds.max_frame_bytes);
    PublicationContext ctx;
    switch (frame.type) {
      case protocol::MessageType::RegisterFederation: {
        const Result<FederationRecord> record =
            protocol::decode_federation_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_federation(ctx, record.value()));
      }
      case protocol::MessageType::RegisterSite: {
        const Result<SiteRecord> record = protocol::decode_site_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_site(ctx, record.value()));
      }
      case protocol::MessageType::RegisterCluster: {
        const Result<ClusterRecord> record = protocol::decode_cluster_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_cluster(ctx, record.value()));
      }
      case protocol::MessageType::RegisterAcceleratorClass: {
        const Result<AcceleratorClassRecord> record =
            protocol::decode_accelerator_class_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_accelerator_class(ctx, record.value()));
      }
      case protocol::MessageType::RegisterRuntime: {
        const Result<RuntimeRecord> record = protocol::decode_runtime_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_runtime(ctx, record.value()));
      }
      case protocol::MessageType::RegisterBackend: {
        const Result<BackendRecord> record = protocol::decode_backend_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_backend(ctx, record.value()));
      }
      case protocol::MessageType::RegisterDomain: {
        const Result<DomainRecord> record = protocol::decode_domain_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_domain(ctx, record.value()));
      }
      case protocol::MessageType::RegisterPolicy: {
        const Result<PolicyRecord> record = protocol::decode_policy_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_policy(ctx, record.value()));
      }
      case protocol::MessageType::RegisterArtifact: {
        const Result<ArtifactRecord> record = protocol::decode_artifact_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_artifact(ctx, record.value()));
      }
      case protocol::MessageType::RegisterWorkloadClass: {
        const Result<WorkloadClassRecord> record =
            protocol::decode_workload_class_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_workload_class(ctx, record.value()));
      }
      case protocol::MessageType::RegisterWorkload: {
        const Result<WorkloadRecord> record = protocol::decode_workload_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.register_workload(ctx, record.value()));
      }
      case protocol::MessageType::PublishPlacement: {
        const Result<PlacementRecord> record = protocol::decode_placement_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.publish_placement(ctx, record.value()));
      }
      case protocol::MessageType::PublishMigration: {
        const Result<MigrationRecord> record = protocol::decode_migration_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.publish_migration(ctx, record.value()));
      }
      case protocol::MessageType::PublishPortability: {
        const Result<PortabilityAssessment> record =
            protocol::decode_portability_publication(decoder, ctx);
        if (!record.ok()) {
          return make_error_reply(frame, record.error().code(), record.error().message(),
                                  record.error().detail());
        }
        return make_publication_reply(frame, obs.publish_portability(ctx, record.value()));
      }
      case protocol::MessageType::PublishCapability: {
        const Result<protocol::CapabilityPublicationRequest> request =
            protocol::decode_capability_publication(decoder);
        if (!request.ok()) {
          return make_error_reply(frame, request.error().code(), request.error().message(),
                                  request.error().detail());
        }
        return make_publication_reply(
            frame, obs.publish_capability(request.value().context, request.value().cluster,
                                          request.value().cluster_generation,
                                          request.value().capability_generation,
                                          request.value().capabilities));
      }
      case protocol::MessageType::PublishCapacity: {
        const Result<protocol::CapacityPublicationRequest> request =
            protocol::decode_capacity_publication(decoder);
        if (!request.ok()) {
          return make_error_reply(frame, request.error().code(), request.error().message(),
                                  request.error().detail());
        }
        return make_publication_reply(
            frame, obs.publish_capacity(request.value().context, request.value().cluster,
                                        request.value().cluster_generation,
                                        request.value().capacity_generation, request.value().pools));
      }
      case protocol::MessageType::PublishMigrationStage: {
        const Result<protocol::MigrationStageRequest> request =
            protocol::decode_migration_stage(decoder);
        if (!request.ok()) {
          return make_error_reply(frame, request.error().code(), request.error().message(),
                                  request.error().detail());
        }
        return make_publication_reply(
            frame, obs.publish_migration_stage(request.value().context, request.value().migration,
                                               request.value().generation, request.value().stage,
                                               request.value().detail));
      }
      case protocol::MessageType::RetireCluster: {
        const Result<protocol::RetireClusterRequest> request =
            protocol::decode_retire_cluster(decoder);
        if (!request.ok()) {
          return make_error_reply(frame, request.error().code(), request.error().message(),
                                  request.error().detail());
        }
        return make_publication_reply(
            frame, obs.retire_cluster(request.value().context, request.value().cluster,
                                      request.value().generation, request.value().reason));
      }
      default:
        break;
    }
    return make_error_reply(frame, ErrorCode::ProtocolViolation, "unsupported publication type",
                            std::string(protocol::to_string(frame.type)));
  }

  if (protocol::is_query(frame.type)) {
    Decoder decoder(frame.payload.data(), frame.payload.size(), config.bounds.max_frame_bytes);
    switch (frame.type) {
      case protocol::MessageType::QueryHealth: {
        const SnapshotHandle handle = obs.snapshot();
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "coordinator health";
        payload.text = handle->health.render("  ");
        payload.rows.push_back(wire_row({{"coordinator_epoch", obs.coordinator_epoch().to_string()},
                                         {"snapshot_generation",
                                          obs.snapshot_generation().to_string()},
                                         {"clusters_total",
                                          std::to_string(handle->health.clusters_total)},
                                         {"clusters_current",
                                          std::to_string(handle->health.clusters_current)},
                                         {"publishers_live",
                                          std::to_string(handle->health.publishers_live)},
                                         {"degraded", handle->health.degraded ? "yes" : "no"}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryBounds: {
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "effective bounds";
        payload.text = render_bounds(config.bounds);
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QuerySnapshot: {
        const Result<protocol::SnapshotQuery> query = protocol::decode_snapshot_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const SnapshotHandle handle = query.value().federation.empty()
                                          ? obs.snapshot()
                                          : obs.snapshot_for(query.value().federation);
        return make_reply(frame, reply_from_snapshot(*handle));
      }
      case protocol::MessageType::QueryPlacement: {
        const Result<protocol::PlacementQuery> query = protocol::decode_placement_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<PlacementExplanation> explanation =
            query.value().latest_by_workload ? obs.explain_latest_placement(query.value().workload)
                                             : obs.explain_placement(query.value().placement);
        if (!explanation.ok()) {
          return make_error_reply(frame, explanation.error().code(), explanation.error().message(),
                                  explanation.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "placement explanation";
        payload.digest = explanation.value().digest();
        payload.text = explanation.value().render();
        payload.rows.push_back(wire_row(
            {{"placement", explanation.value().placement.value()},
             {"selected", explanation.value().selected.value()},
             {"candidate_completeness",
              std::string(fo::to_string(explanation.value().candidate_completeness))},
             {"rejection_attribution_available",
              explanation.value().has_rejection_attribution() ? "yes" : "no"},
             {"currentness", std::string(fo::to_string(explanation.value().currentness))},
             {"precision", std::string(fo::to_string(explanation.value().precision))}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryRejection: {
        const Result<protocol::RejectionQuery> query = protocol::decode_rejection_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<RejectionExplanation> explanation =
            obs.explain_rejection(query.value().placement, query.value().candidate);
        if (!explanation.ok()) {
          return make_error_reply(frame, explanation.error().code(), explanation.error().message(),
                                  explanation.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "rejection explanation";
        payload.digest = explanation.value().digest();
        payload.text = explanation.value().render();
        payload.rows.push_back(wire_row(
            {{"candidate", explanation.value().candidate.value()},
             {"attribution_available", explanation.value().attribution_available ? "yes" : "no"},
             {"reasons", std::to_string(explanation.value().reasons.size())}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryStrandedCapacity: {
        const Result<protocol::StrandedQuery> query = protocol::decode_stranded_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<StrandedCapacityReport> report =
            obs.stranded_capacity(query.value().request);
        if (!report.ok()) {
          return make_error_reply(frame, report.error().code(), report.error().message(),
                                  report.error().detail());
        }
        return make_reply(frame, reply_from_stranded(report.value()));
      }
      case protocol::MessageType::QueryFragmentation: {
        const Result<protocol::FragmentationQuery> query =
            protocol::decode_fragmentation_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<FragmentationFinding> finding =
            obs.fragmentation(query.value().federation, query.value().workload_class,
                              query.value().kind);
        if (!finding.ok()) {
          return make_error_reply(frame, finding.error().code(), finding.error().message(),
                                  finding.error().detail());
        }
        return make_reply(frame, reply_from_fragmentation(finding.value()));
      }
      case protocol::MessageType::QueryCompatibility: {
        const Result<protocol::CompatibilityQuery> query =
            protocol::decode_compatibility_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<CompatibilityAssessment> assessment =
            obs.compatibility(query.value().cluster, query.value().workload);
        if (!assessment.ok()) {
          return make_error_reply(frame, assessment.error().code(), assessment.error().message(),
                                  assessment.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "compatibility assessment";
        payload.digest = digest_text(assessment.value().render());
        payload.text = assessment.value().render();
        payload.rows.push_back(wire_row(
            {{"cluster", assessment.value().target_cluster.value()},
             {"workload", assessment.value().workload.value()},
             {"overall", std::string(fo::to_string(assessment.value().overall))},
             {"admits_placement", assessment.value().admits_placement() ? "yes" : "no"},
             {"requires_adaptation", assessment.value().requires_adaptation() ? "yes" : "no"},
             {"policy_admissible", std::string(fo::to_string(assessment.value().policy_admissible))},
             {"checks", std::to_string(assessment.value().checks.size())}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryPortability: {
        const Result<protocol::PortabilityQuery> query =
            protocol::decode_portability_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<PortabilityAssessment> assessment =
            query.value().evaluate
                ? obs.evaluate_portability(query.value().workload, query.value().destination)
                : obs.portability(query.value().workload, query.value().destination);
        if (!assessment.ok()) {
          return make_error_reply(frame, assessment.error().code(), assessment.error().message(),
                                  assessment.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "portability assessment";
        payload.digest = assessment.value().digest();
        payload.text = assessment.value().render();
        payload.rows.push_back(wire_row(
            {{"workload", assessment.value().workload.value()},
             {"destination", assessment.value().destination.value()},
             {"overall", std::string(fo::to_string(assessment.value().overall))},
             {"technically_blocked", assessment.value().technically_blocked ? "yes" : "no"},
             {"policy_blocked", assessment.value().policy_blocked ? "yes" : "no"},
             {"dimensions", std::to_string(assessment.value().dimensions.size())}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryMigration: {
        const Result<protocol::MigrationQuery> query = protocol::decode_migration_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<MigrationAnalysis> analysis = obs.migration_analysis(query.value().migration);
        if (!analysis.ok()) {
          return make_error_reply(frame, analysis.error().code(), analysis.error().message(),
                                  analysis.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "migration analysis";
        payload.digest = analysis.value().digest();
        payload.text = analysis.value().render();
        payload.rows.push_back(wire_row(
            {{"migration", analysis.value().migration.value()},
             {"stage", std::string(fo::to_string(analysis.value().stage))},
             {"outcome", std::string(fo::to_string(analysis.value().outcome))},
             {"superseded", analysis.value().superseded ? "yes" : "no"},
             {"state_portability_proven",
              std::string(fo::to_string(analysis.value().state_portability_proven))},
             {"revalidation_outstanding",
              analysis.value().revalidation_outstanding ? "yes" : "no"}}));
        return make_reply(frame, std::move(payload));
      }
      case protocol::MessageType::QueryMismatch: {
        const Result<protocol::MismatchQuery> query = protocol::decode_mismatch_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        const Result<MismatchAnalysis> analysis = obs.mismatch_analysis(query.value().request);
        if (!analysis.ok()) {
          return make_error_reply(frame, analysis.error().code(), analysis.error().message(),
                                  analysis.error().detail());
        }
        return make_reply(frame, reply_from_mismatch(analysis.value()));
      }
      case protocol::MessageType::QueryDrift: {
        const Result<protocol::DriftQuery> query = protocol::decode_drift_query(decoder);
        if (!query.ok()) {
          return make_error_reply(frame, query.error().code(), query.error().message(),
                                  query.error().detail());
        }
        DriftRequest request;
        request.federation = query.value().federation;
        request.intended = query.value().have_intended ? &query.value().intended : nullptr;
        request.behavior_window_supplied = query.value().behavior_window_supplied;
        request.before_window = query.value().before_window;
        request.after_window = query.value().after_window;
        const Result<DriftReport> report = obs.drift(request);
        if (!report.ok()) {
          return make_error_reply(frame, report.error().code(), report.error().message(),
                                  report.error().detail());
        }
        protocol::ReplyPayload payload;
        payload.code = ErrorCode::Ok;
        payload.message = "drift report";
        payload.digest = report.value().digest();
        payload.text = report.value().render();
        payload.rows.push_back(wire_row(
            {{"intended_state_supplied", report.value().intended_state_supplied ? "yes" : "no"},
             {"behavior_window_supplied",
              report.value().behavior_window_supplied ? "yes" : "no"},
             {"findings", std::to_string(report.value().findings.size())}}));
        return make_reply(frame, std::move(payload));
      }
      default:
        break;
    }
    return make_error_reply(frame, ErrorCode::ProtocolViolation, "unsupported query type",
                            std::string(protocol::to_string(frame.type)));
  }

  if (frame.type == protocol::MessageType::Shutdown) {
    shutdown_requested.store(true);
    protocol::ReplyPayload payload;
    payload.code = ErrorCode::Ok;
    payload.message = "shutdown acknowledged; the coordinator will stop after this reply";
    return make_reply(frame, std::move(payload));
  }

  (void)peer;
  return make_error_reply(frame, ErrorCode::ProtocolViolation, "message type is not accepted here",
                          std::string(protocol::to_string(frame.type)));
}

FederationCoordinator::FederationCoordinator(CoordinatorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

FederationCoordinator::~FederationCoordinator() { const Status stopped = stop(); (void)stopped; }

Status FederationCoordinator::start() {
  if (impl_->running.load()) {
    return Status::success();
  }
  const Status valid = impl_->config.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  const Status sockets = net::initialize_sockets();
  if (!sockets.ok()) {
    return sockets.error();
  }
  impl_->coordinator_id = "coordinator-" + std::to_string(now_unix_nanos());

  if (!impl_->config.state_path.empty()) {
    const Status loaded = load_state(impl_->config.state_path);
    if (!loaded.ok() && loaded.code() != ErrorCode::NotFound) {
      return loaded;
    }
  }

  Result<net::TcpListener> listener =
      net::TcpListener::bind(impl_->config.bind_host, impl_->config.port, 64);
  if (!listener.ok()) {
    return listener.error();
  }
  impl_->listener = listener.take();
  impl_->stopping.store(false);
  impl_->shutdown_requested.store(false);
  impl_->running.store(true);

  const std::size_t accept_threads = impl_->config.accept_threads;
  for (std::size_t i = 0; i < accept_threads; ++i) {
    impl_->accept_threads.emplace_back([this] {
      while (!impl_->stopping.load()) {
        Result<net::Socket> accepted = impl_->listener.accept(impl_->config.poll_millis);
        if (!accepted.ok()) {
          if (accepted.code() == ErrorCode::ConnectionClosed) {
            break;
          }
          continue;
        }
        if (!accepted.value().valid()) {
          continue;
        }
        auto socket = std::make_shared<net::Socket>(accepted.take());
        std::uint64_t id = 0;
        {
          std::lock_guard<std::mutex> lock(impl_->connections_mutex);
          if (impl_->connections.size() >= impl_->config.max_connections) {
            std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
            impl_->counters.connections_rejected += 1;
            socket->close();
            continue;
          }
          id = ++impl_->next_connection_id;
          impl_->connections.emplace(id, socket);
          std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
          impl_->counters.connections_accepted += 1;
          impl_->counters.active_connections = impl_->connections.size();
        }
        const std::string peer = socket->peer_address();
        impl_->connection_threads.emplace_back([this, socket, peer, id] {
          const Status served = [&] {
            protocol::FrameReader reader(*socket, impl_->config.bounds);
            for (;;) {
              if (impl_->stopping.load()) {
                return Status::success();
              }
              Result<protocol::Frame> frame = reader.next(impl_->config.poll_millis);
              if (!frame.ok()) {
                if (frame.code() == ErrorCode::ConnectionClosed) {
                  return Status::success();
                }
                std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
                impl_->counters.frames_rejected += 1;
                return Status::success();
              }
              if (frame.value().type == protocol::MessageType::Invalid) {
                continue;
              }
              {
                std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
                impl_->counters.frames_received += 1;
              }
              auto item = std::make_shared<Impl::WorkItem>();
              item->frame = std::move(frame.value());
              item->peer = peer;
              auto completion = item->completion.get_future();
              bool queued = false;
              {
                std::lock_guard<std::mutex> lock(impl_->queue_mutex);
                if (impl_->queue.size() < impl_->config.bounds.max_ingest_queue_depth &&
                    !impl_->stopping.load()) {
                  impl_->queue.push_back(item);
                  queued = true;
                }
              }
              if (!queued) {
                {
                  std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
                  impl_->counters.queue_rejections += 1;
                }
                const protocol::Frame refused = make_error_reply(
                    item->frame, ErrorCode::Backpressure,
                    "the coordinator ingest queue is full; this observation was refused and was NOT "
                    "applied, so the published evidence is known-incomplete");
                const Result<std::vector<std::uint8_t>> image =
                    protocol::encode_frame(refused, impl_->config.bounds);
                if (image.ok() && !socket->write_all(image.value().data(), image.value().size()).ok()) {
                  return Status::success();
                }
                continue;
              }
              impl_->queue_cv.notify_one();
              Result<protocol::Frame> reply = completion.get();
              if (!reply.ok()) {
                const protocol::Frame refused =
                    make_error_reply(item->frame, reply.error().code(), reply.error().message(),
                                     reply.error().detail());
                const Result<std::vector<std::uint8_t>> image =
                    protocol::encode_frame(refused, impl_->config.bounds);
                if (!image.ok() ||
                    !socket->write_all(image.value().data(), image.value().size()).ok()) {
                  return Status::success();
                }
                continue;
              }
              if (impl_->stopping.load()) {
                return Status::success();
              }
              const Result<std::vector<std::uint8_t>> image =
                  protocol::encode_frame(reply.value(), impl_->config.bounds);
              if (!image.ok()) {
                std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
                impl_->counters.frames_rejected += 1;
                return Status::success();
              }
              const Status written =
                  socket->write_all(image.value().data(), image.value().size());
              if (!written.ok()) {
                return Status::success();
              }
              {
                std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
                impl_->counters.frames_sent += 1;
              }
            }
          }();
          (void)served;
          socket->close();
          std::lock_guard<std::mutex> lock(impl_->connections_mutex);
          impl_->connections.erase(id);
          std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
          impl_->counters.active_connections = impl_->connections.size();
        });
      }
    });
  }

  for (std::size_t i = 0; i < impl_->config.worker_threads; ++i) {
    impl_->ingest_threads.emplace_back([this] {
      for (;;) {
        std::shared_ptr<Impl::WorkItem> item;
        {
          std::unique_lock<std::mutex> lock(impl_->queue_mutex);
          impl_->queue_cv.wait(lock, [this] {
            return !impl_->queue.empty() || impl_->stopping.load();
          });
          if (impl_->queue.empty()) {
            if (impl_->stopping.load()) {
              return;
            }
            continue;
          }
          item = impl_->queue.front();
          impl_->queue.pop_front();
        }
        Result<protocol::Frame> reply = impl_->dispatch(item->frame, item->peer);
        item->completion.set_value(std::move(reply));
      }
    });
  }

  if (!impl_->config.ready_file.empty()) {
    const Status written = write_text_file(
        impl_->config.ready_file, std::to_string(impl_->listener.port()));
    if (!written.ok()) {
      const Status stopped = stop();
      (void)stopped;
      return written;
    }
  }
  impl_->log_line("coordinator started on port " + std::to_string(impl_->listener.port()) +
                  " epoch " + impl_->observatory->coordinator_epoch().to_string());
  return Status::success();
}

Status FederationCoordinator::stop() {
  if (!impl_->running.load()) {
    return Status::success();
  }
  impl_->stopping.store(true);
  impl_->listener.close();
  impl_->queue_cv.notify_all();

  // Release every connection thread that is blocked on a socket read by closing its
  // socket. The threads then observe a closed connection and return normally.
  {
    std::lock_guard<std::mutex> lock(impl_->connections_mutex);
    for (auto& entry : impl_->connections) {
      entry.second->close();
      std::lock_guard<std::mutex> counters_lock(impl_->counters_mutex);
      impl_->counters.connections_closed_by_shutdown += 1;
    }
  }

  const std::thread::id self = std::this_thread::get_id();
  const auto join_all = [self](std::vector<std::thread>& threads) {
    for (std::thread& thread : threads) {
      if (thread.joinable()) {
        if (thread.get_id() == self) {
          // Never self-join: a coordinator stopped from one of its own threads
          // detaches that thread and completes shutdown on the caller's.
          thread.detach();
          continue;
        }
        thread.join();
      }
    }
    threads.clear();
  };

  // Any work item still queued when the ingest workers exit must be answered, or its
  // connection thread would wait forever on a promise that nobody will fulfil.
  for (;;) {
    std::shared_ptr<Impl::WorkItem> leftover;
    {
      std::lock_guard<std::mutex> lock(impl_->queue_mutex);
      if (impl_->queue.empty()) {
        break;
      }
      leftover = impl_->queue.front();
      impl_->queue.pop_front();
    }
    leftover->completion.set_value(Error(ErrorCode::ShuttingDown,
                                        "the coordinator stopped before this observation was "
                                        "applied; it was NOT applied"));
  }

  join_all(impl_->ingest_threads);
  join_all(impl_->connection_threads);
  join_all(impl_->accept_threads);

  if (!impl_->config.state_path.empty()) {
    const Status saved = save_state(impl_->config.state_path);
    if (!saved.ok()) {
      impl_->log_line("state save on shutdown failed: " + saved.error().to_string());
    }
  }
  impl_->running.store(false);
  impl_->stopping.store(false);
  impl_->log_line("coordinator stopped");
  return Status::success();
}

bool FederationCoordinator::running() const noexcept { return impl_->running.load(); }

bool FederationCoordinator::shutdown_requested() const noexcept {
  return impl_->shutdown_requested.load();
}

std::uint16_t FederationCoordinator::port() const noexcept { return impl_->listener.port(); }

const std::string& FederationCoordinator::bind_host() const noexcept {
  return impl_->config.bind_host;
}

FederationObservatory& FederationCoordinator::observatory() { return *impl_->observatory; }

const FederationObservatory& FederationCoordinator::observatory() const {
  return *impl_->observatory;
}

Status FederationCoordinator::save_state(const std::string& path) {
  return impl_->observatory->save_state(path);
}

Status FederationCoordinator::load_state(const std::string& path) {
  return impl_->observatory->load_state(path);
}

Result<protocol::Frame> FederationCoordinator::handle_frame(const protocol::Frame& frame,
                                                            const std::string& peer) {
  if (!frame.is_request()) {
    return Error(ErrorCode::ProtocolViolation, "frame is not a request");
  }
  return impl_->dispatch(frame, peer);
}

FederationCoordinator::Counters FederationCoordinator::counters() const {
  std::lock_guard<std::mutex> lock(impl_->counters_mutex);
  return impl_->counters;
}

std::string FederationCoordinator::stats_report() const {
  const Counters snapshot = counters();
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"coordinator_epoch", impl_->observatory->coordinator_epoch().to_string()});
  rows.push_back({"snapshot_generation", impl_->observatory->snapshot_generation().to_string()});
  rows.push_back({"connections_accepted", std::to_string(snapshot.connections_accepted)});
  rows.push_back({"connections_rejected", std::to_string(snapshot.connections_rejected)});
  rows.push_back({"active_connections", std::to_string(snapshot.active_connections)});
  rows.push_back({"frames_received", std::to_string(snapshot.frames_received)});
  rows.push_back({"frames_rejected", std::to_string(snapshot.frames_rejected)});
  rows.push_back({"frames_sent", std::to_string(snapshot.frames_sent)});
  rows.push_back({"queue_rejections", std::to_string(snapshot.queue_rejections)});
  rows.push_back(
      {"connections_closed_by_shutdown", std::to_string(snapshot.connections_closed_by_shutdown)});
  return render_table({"counter", "value"}, rows, "");
}

namespace {

void print_coordinator_usage() {
  std::printf(
      "fo-coordinator: run a Federation Observatory coordinator process.\n"
      "\n"
      "usage: fo-coordinator [options]\n"
      "  --host <host>          bind host (default 127.0.0.1)\n"
      "  --port <port>          bind port; 0 selects an ephemeral port (default 0)\n"
      "  --state <path>         durable state file to restore on start and save on stop\n"
      "  --ready-file <path>    write the bound port here once the listener is up\n"
      "  --threads <n>          ingest worker threads (default 1; 1 preserves global order)\n"
      "  --poll-ms <n>          server-side poll quantum in milliseconds (default 50)\n"
      "  --admin-token <token>  shared secret required by FENCE\n"
      "  --max-connections <n>  hard cap on concurrent connections\n"
      "  --no-fence             refuse every remote FENCE\n"
      "  --log <path>           append an operational log\n"
      "  --stats-file <path>    write a final stats report on shutdown\n");
}

}  // namespace

int run_coordinator_main(int argc, char** argv) {
  CoordinatorConfig config;
  std::string stats_file;
  for (int i = 1; i < argc; ++i) {
    const std::string flag(argv[i]);
    const auto next = [&](std::string& target) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      target = argv[++i];
      return true;
    };
    std::string value;
    if (flag == "--help" || flag == "-h") {
      print_coordinator_usage();
      return 0;
    } else if (flag == "--host") {
      if (!next(value)) {
        std::printf("fo-coordinator: --host requires a value\n");
        return 2;
      }
      config.bind_host = value;
    } else if (flag == "--port") {
      if (!next(value)) {
        std::printf("fo-coordinator: --port requires a value\n");
        return 2;
      }
      const long port = std::strtol(value.c_str(), nullptr, 10);
      if (port < 0 || port > 65535) {
        std::printf("fo-coordinator: --port is out of range\n");
        return 2;
      }
      config.port = static_cast<std::uint16_t>(port);
    } else if (flag == "--state") {
      if (!next(value)) {
        std::printf("fo-coordinator: --state requires a value\n");
        return 2;
      }
      config.state_path = value;
    } else if (flag == "--ready-file") {
      if (!next(value)) {
        std::printf("fo-coordinator: --ready-file requires a value\n");
        return 2;
      }
      config.ready_file = value;
    } else if (flag == "--threads") {
      if (!next(value)) {
        std::printf("fo-coordinator: --threads requires a value\n");
        return 2;
      }
      config.worker_threads = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--poll-ms") {
      if (!next(value)) {
        std::printf("fo-coordinator: --poll-ms requires a value\n");
        return 2;
      }
      config.poll_millis = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
    } else if (flag == "--admin-token") {
      if (!next(value)) {
        std::printf("fo-coordinator: --admin-token requires a value\n");
        return 2;
      }
      config.admin_token = value;
    } else if (flag == "--max-connections") {
      if (!next(value)) {
        std::printf("fo-coordinator: --max-connections requires a value\n");
        return 2;
      }
      config.max_connections = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (flag == "--no-fence") {
      config.allow_fence = false;
    } else if (flag == "--log") {
      if (!next(value)) {
        std::printf("fo-coordinator: --log requires a value\n");
        return 2;
      }
      config.log_path = value;
    } else if (flag == "--stats-file") {
      if (!next(value)) {
        std::printf("fo-coordinator: --stats-file requires a value\n");
        return 2;
      }
      stats_file = value;
    } else {
      std::printf("fo-coordinator: unrecognised option %s\n", flag.c_str());
      print_coordinator_usage();
      return 2;
    }
  }

  FederationCoordinator coordinator(config);
  const Status started = coordinator.start();
  if (!started.ok()) {
    std::printf("fo-coordinator: %s\n", started.error().to_string().c_str());
    return 3;
  }
  std::printf("fo-coordinator: listening on %s:%u epoch %s\n", config.bind_host.c_str(),
              static_cast<unsigned>(coordinator.port()),
              coordinator.observatory().coordinator_epoch().to_string().c_str());
  std::fflush(stdout);

  // The coordinator serves until it is asked to stop through its own protocol or the
  // hosting process terminates it. A closed standard input is NOT a stop request: a
  // coordinator started without a console must keep serving rather than exit immediately.
  while (!coordinator.shutdown_requested()) {
    sleep_millis(50);
  }

  if (!stats_file.empty()) {
    const Status written = write_text_file(stats_file, coordinator.stats_report());
    (void)written;
  }
  const Status stopped = coordinator.stop();
  if (!stopped.ok()) {
    std::printf("fo-coordinator: shutdown reported %s\n", stopped.error().to_string().c_str());
    return 5;
  }
  std::printf("fo-coordinator: stopped cleanly\n");
  return 0;
}

}  // namespace fo
