// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// A synchronous client of a remote coordinator. Every method performs exactly one
// request/response exchange; nothing is cached locally, so an inspection can never
// report state the coordinator did not just confirm.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/client.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/protocol.hpp"

namespace fo {

class FederationClient::Impl {
 public:
  ClientConfig config;
  net::Socket socket;
  std::unique_ptr<protocol::FrameReader> reader;
  std::uint64_t next_request_id = 0;
  std::string peer;
  bool connected = false;

  [[nodiscard]] Result<protocol::Frame> exchange(protocol::MessageType type,
                                                 const std::vector<std::uint8_t>& payload) {
    if (!connected || !socket.valid()) {
      return Error(ErrorCode::ConnectionClosed, "client is not connected");
    }
    protocol::Frame frame;
    frame.type = type;
    frame.flags = protocol::frame_flag(protocol::FrameFlags::Request);
    frame.request_id = ++next_request_id;
    frame.payload = payload;
    const Result<std::vector<std::uint8_t>> image = protocol::encode_frame(frame, config.bounds);
    if (!image.ok()) {
      return image.error();
    }
    const Status written =
        socket.write_all(image.value().data(), image.value().size());
    if (!written.ok()) {
      connected = false;
      return written.error();
    }
    for (;;) {
      const Result<protocol::Frame> reply = reader->next(config.poll_millis);
      if (!reply.ok()) {
        if (reply.code() == ErrorCode::ConnectionClosed) {
          connected = false;
        }
        return reply.error();
      }
      if (reply.value().type == protocol::MessageType::Invalid) {
        // The poll quantum expired with a partial frame buffered; keep waiting.
        continue;
      }
      if (reply.value().request_id != frame.request_id) {
        return Error(ErrorCode::ProtocolViolation,
                     "reply does not correspond to the outstanding request",
                     std::to_string(reply.value().request_id));
      }
      return reply.value();
    }
  }

  [[nodiscard]] Result<protocol::PublicationAck> publication(
      protocol::MessageType type, const std::vector<std::uint8_t>& payload) {
    const Result<protocol::Frame> reply = exchange(type, payload);
    if (!reply.ok()) {
      return reply.error();
    }
    if (reply.value().type == protocol::MessageType::Failure) {
      Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                      config.bounds.max_frame_bytes);
      const Result<protocol::ReplyPayload> failure = protocol::decode_reply(decoder);
      if (failure.ok()) {
        return Error(failure.value().code, failure.value().message, failure.value().detail);
      }
      return Error(ErrorCode::ProtocolViolation, "coordinator returned an undecodable failure");
    }
    if (reply.value().type != protocol::MessageType::Reply) {
      return Error(ErrorCode::ProtocolViolation, "unexpected reply type",
                   std::string(protocol::to_string(reply.value().type)));
    }
    Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                    config.bounds.max_frame_bytes);
    return protocol::decode_publication_ack(decoder);
  }

  [[nodiscard]] Result<protocol::ReplyPayload> query(protocol::MessageType type,
                                                     const std::vector<std::uint8_t>& payload) {
    const Result<protocol::Frame> reply = exchange(type, payload);
    if (!reply.ok()) {
      return reply.error();
    }
    if (reply.value().type != protocol::MessageType::Reply &&
        reply.value().type != protocol::MessageType::Failure) {
      return Error(ErrorCode::ProtocolViolation, "unexpected reply type",
                   std::string(protocol::to_string(reply.value().type)));
    }
    Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                    config.bounds.max_frame_bytes);
    Result<protocol::ReplyPayload> decoded = protocol::decode_reply(decoder);
    if (!decoded.ok()) {
      return decoded.error();
    }
    if (reply.value().type == protocol::MessageType::Failure || decoded.value().code != ErrorCode::Ok) {
      return Error(decoded.value().code, decoded.value().message, decoded.value().detail);
    }
    return decoded.take();
  }
};

FederationClient::FederationClient() : impl_(nullptr) {}
FederationClient::~FederationClient() = default;
FederationClient::FederationClient(FederationClient&&) noexcept = default;
FederationClient& FederationClient::operator=(FederationClient&&) noexcept = default;

Result<FederationClient> FederationClient::connect(const ClientConfig& config) {
  const Status sockets = net::initialize_sockets();
  if (!sockets.ok()) {
    return sockets.error();
  }
  const Status valid = config.bounds.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  Result<net::Socket> socket = net::connect(config.host, config.port);
  if (!socket.ok()) {
    return socket.error();
  }
  const Status nodelay = socket.value().set_nodelay(true);
  (void)nodelay;
  FederationClient client;
  client.impl_ = std::make_unique<Impl>();
  client.impl_->config = config;
  client.impl_->peer = socket.value().peer_address();
  client.impl_->socket = socket.take();
  client.impl_->reader =
      std::make_unique<protocol::FrameReader>(client.impl_->socket, client.impl_->config.bounds);
  client.impl_->connected = true;
  return client;
}

bool FederationClient::connected() const noexcept {
  return impl_ != nullptr && impl_->connected && impl_->socket.valid();
}

const std::string& FederationClient::peer() const noexcept {
  static const std::string kEmpty;
  return impl_ != nullptr ? impl_->peer : kEmpty;
}

Status FederationClient::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  if (impl_->connected) {
    const Status sent = impl_->socket.shutdown_send();
    (void)sent;
  }
  impl_->connected = false;
  impl_->socket.close();
  return Status::success();
}

Result<protocol::HelloReply> FederationClient::hello(const PublisherId& publisher) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::HelloRequest request;
  request.protocol = static_cast<std::uint16_t>(protocol::kProtocolVersion);
  request.client_version = impl_->config.client_version;
  request.publisher = publisher;
  Encoder encoder;
  protocol::encode(encoder, request);
  const Result<protocol::Frame> reply =
      impl_->exchange(protocol::MessageType::Hello, encoder.take());
  if (!reply.ok()) {
    return reply.error();
  }
  if (reply.value().type != protocol::MessageType::HelloAck) {
    if (reply.value().type == protocol::MessageType::Failure) {
      Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                      impl_->config.bounds.max_frame_bytes);
      const Result<protocol::ReplyPayload> failure = protocol::decode_reply(decoder);
      if (failure.ok()) {
        return Error(failure.value().code, failure.value().message, failure.value().detail);
      }
    }
    return Error(ErrorCode::ProtocolViolation, "handshake was not acknowledged");
  }
  Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                  impl_->config.bounds.max_frame_bytes);
  return protocol::decode_hello_reply(decoder);
}

Result<protocol::PublicationAck> FederationClient::register_publisher(const PublicationContext& ctx,
                                                                      std::string role) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::RegisterPublisherRequest request;
  request.context = ctx;
  request.role = std::move(role);
  Encoder encoder;
  protocol::encode(encoder, request);
  return impl_->publication(protocol::MessageType::RegisterPublisher, encoder.take());
}

#define FO_CLIENT_REGISTRATION(MethodName, MessageTypeName, RequestField)                    \
  Result<protocol::PublicationAck> FederationClient::MethodName(const PublicationContext& ctx, \
                                                                RequestField record) {        \
    if (impl_ == nullptr) {                                                                   \
      return Error(ErrorCode::ConnectionClosed, "client is not connected");                   \
    }                                                                                         \
    Encoder encoder;                                                                          \
    protocol::encode_publication(encoder, ctx, record);                                       \
    return impl_->publication(protocol::MessageType::MessageTypeName, encoder.take());        \
  }

FO_CLIENT_REGISTRATION(register_federation, RegisterFederation, FederationRecord)
FO_CLIENT_REGISTRATION(register_site, RegisterSite, SiteRecord)
FO_CLIENT_REGISTRATION(register_cluster, RegisterCluster, ClusterRecord)
FO_CLIENT_REGISTRATION(register_accelerator_class, RegisterAcceleratorClass, AcceleratorClassRecord)
FO_CLIENT_REGISTRATION(register_runtime, RegisterRuntime, RuntimeRecord)
FO_CLIENT_REGISTRATION(register_backend, RegisterBackend, BackendRecord)
FO_CLIENT_REGISTRATION(register_domain, RegisterDomain, DomainRecord)
FO_CLIENT_REGISTRATION(register_policy, RegisterPolicy, PolicyRecord)
FO_CLIENT_REGISTRATION(register_artifact, RegisterArtifact, ArtifactRecord)
FO_CLIENT_REGISTRATION(register_workload_class, RegisterWorkloadClass, WorkloadClassRecord)
FO_CLIENT_REGISTRATION(register_workload, RegisterWorkload, WorkloadRecord)
FO_CLIENT_REGISTRATION(publish_placement, PublishPlacement, PlacementRecord)
FO_CLIENT_REGISTRATION(publish_migration, PublishMigration, MigrationRecord)
FO_CLIENT_REGISTRATION(publish_portability, PublishPortability, PortabilityAssessment)

#undef FO_CLIENT_REGISTRATION

Result<protocol::PublicationAck> FederationClient::publish_capability(
    const PublicationContext& ctx, const ClusterId& cluster, ClusterGeneration cluster_generation,
    AcceleratorCapabilityGeneration capability_generation, CapabilitySet capabilities) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::CapabilityPublicationRequest request;
  request.context = ctx;
  request.cluster = cluster;
  request.cluster_generation = cluster_generation;
  request.capability_generation = capability_generation;
  request.capabilities = std::move(capabilities);
  Encoder encoder;
  protocol::encode(encoder, request);
  return impl_->publication(protocol::MessageType::PublishCapability, encoder.take());
}

Result<protocol::PublicationAck> FederationClient::publish_capacity(
    const PublicationContext& ctx, const ClusterId& cluster, ClusterGeneration cluster_generation,
    CapacityGeneration capacity_generation, std::vector<CapacityPool> pools) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::CapacityPublicationRequest request;
  request.context = ctx;
  request.cluster = cluster;
  request.cluster_generation = cluster_generation;
  request.capacity_generation = capacity_generation;
  request.pools = std::move(pools);
  Encoder encoder;
  protocol::encode(encoder, request);
  return impl_->publication(protocol::MessageType::PublishCapacity, encoder.take());
}

Result<protocol::PublicationAck> FederationClient::publish_migration_stage(
    const PublicationContext& ctx, const MigrationId& migration, MigrationGeneration generation,
    MigrationStage stage, std::string detail) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::MigrationStageRequest request;
  request.context = ctx;
  request.migration = migration;
  request.generation = generation;
  request.stage = stage;
  request.detail = std::move(detail);
  Encoder encoder;
  protocol::encode(encoder, request);
  return impl_->publication(protocol::MessageType::PublishMigrationStage, encoder.take());
}

Result<protocol::PublicationAck> FederationClient::retire_cluster(const PublicationContext& ctx,
                                                                  const ClusterId& cluster,
                                                                  ClusterGeneration generation,
                                                                  std::string reason) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::RetireClusterRequest request;
  request.context = ctx;
  request.cluster = cluster;
  request.generation = generation;
  request.reason = std::move(reason);
  Encoder encoder;
  protocol::encode(encoder, request);
  return impl_->publication(protocol::MessageType::RetireCluster, encoder.take());
}

Result<protocol::FenceReply> FederationClient::fence_publisher(const PublisherId& publisher,
                                                               BootGeneration boot,
                                                               std::string reason,
                                                               std::string token) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  protocol::FenceRequest request;
  request.publisher = publisher;
  request.boot = boot;
  request.reason = std::move(reason);
  request.token = std::move(token);
  Encoder encoder;
  protocol::encode(encoder, request);
  const Result<protocol::Frame> reply =
      impl_->exchange(protocol::MessageType::FencePublisher, encoder.take());
  if (!reply.ok()) {
    return reply.error();
  }
  Decoder decoder(reply.value().payload.data(), reply.value().payload.size(),
                  impl_->config.bounds.max_frame_bytes);
  const Result<protocol::ReplyPayload> decoded = protocol::decode_reply(decoder);
  if (!decoded.ok()) {
    return decoded.error();
  }
  if (decoded.value().code != ErrorCode::Ok) {
    return Error(decoded.value().code, decoded.value().message, decoded.value().detail);
  }
  protocol::FenceReply fence;
  fence.fenced = true;
  fence.detail = decoded.value().message;
  return fence;
}

#define FO_CLIENT_QUERY(MethodName, MessageTypeName, QueryType)                    \
  Result<protocol::ReplyPayload> FederationClient::MethodName(const QueryType& query) { \
    if (impl_ == nullptr) {                                                        \
      return Error(ErrorCode::ConnectionClosed, "client is not connected");        \
    }                                                                              \
    Encoder encoder;                                                               \
    protocol::encode(encoder, query);                                              \
    return impl_->query(protocol::MessageType::MessageTypeName, encoder.take());   \
  }

FO_CLIENT_QUERY(query_snapshot, QuerySnapshot, protocol::SnapshotQuery)
FO_CLIENT_QUERY(query_placement, QueryPlacement, protocol::PlacementQuery)
FO_CLIENT_QUERY(query_rejection, QueryRejection, protocol::RejectionQuery)
FO_CLIENT_QUERY(query_stranded_capacity, QueryStrandedCapacity, protocol::StrandedQuery)
FO_CLIENT_QUERY(query_fragmentation, QueryFragmentation, protocol::FragmentationQuery)
FO_CLIENT_QUERY(query_compatibility, QueryCompatibility, protocol::CompatibilityQuery)
FO_CLIENT_QUERY(query_portability, QueryPortability, protocol::PortabilityQuery)
FO_CLIENT_QUERY(query_migration, QueryMigration, protocol::MigrationQuery)
FO_CLIENT_QUERY(query_mismatch, QueryMismatch, protocol::MismatchQuery)
FO_CLIENT_QUERY(query_drift, QueryDrift, protocol::DriftQuery)

#undef FO_CLIENT_QUERY

Result<protocol::ReplyPayload> FederationClient::request_shutdown() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  return impl_->query(protocol::MessageType::Shutdown, {});
}

Result<protocol::ReplyPayload> FederationClient::query_health() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  return impl_->query(protocol::MessageType::QueryHealth, {});
}

Result<protocol::ReplyPayload> FederationClient::query_bounds() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::ConnectionClosed, "client is not connected");
  }
  return impl_->query(protocol::MessageType::QueryBounds, {});
}

}  // namespace fo
