// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Bounded, versioned framing. Every rejection happens before any payload field is
// interpreted, so a malformed frame can never reach the state engine.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "federation_observatory/protocol.hpp"
#include "federation_observatory/records_codec.hpp"

namespace fo::protocol {
namespace {

void write_u16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void write_u32(std::uint8_t* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void write_u64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

std::uint16_t read_u16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8));
}

std::uint32_t read_u32(const std::uint8_t* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

bool known_message_type(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(MessageType::Hello) &&
         raw <= static_cast<std::uint16_t>(MessageType::Shutdown) &&
         raw != static_cast<std::uint16_t>(MessageType::Invalid);
}

void encode_publication_context(Encoder& out, const PublicationContext& context) {
  out.str(context.publisher.value());
  out.u64(context.boot.value());
  out.u64(context.coordinator_epoch.value());
  out.str(context.federation.value());
  out.u64(context.federation_generation.value());
  out.u64(context.sequence.value());
  out.i64(context.observed_at);
  out.u8(static_cast<std::uint8_t>(context.precision));
  out.u8(static_cast<std::uint8_t>(context.evidence_class));
  out.u8(static_cast<std::uint8_t>(context.provenance));
  out.u64(context.evidence_generation.value());
}

Result<PublicationContext> decode_publication_context(Decoder& in) {
  PublicationContext context;
  Result<std::string> publisher = in.str();
  if (!publisher.ok()) return publisher.error();
  if (!publisher.value().empty() && !is_valid_identifier(publisher.value())) {
    return Error(ErrorCode::ProtocolViolation, "publication context carries a malformed publisher");
  }
  context.publisher = PublisherId::unchecked(publisher.take());
  Result<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return boot.error();
  context.boot = BootGeneration(boot.value());
  Result<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return epoch.error();
  context.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation,
                 "publication context carries a malformed federation");
  }
  context.federation = FederationId::unchecked(federation.take());
  Result<std::uint64_t> federation_generation = in.u64();
  if (!federation_generation.ok()) return federation_generation.error();
  context.federation_generation = FederationGeneration(federation_generation.value());
  Result<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return sequence.error();
  context.sequence = Sequence(sequence.value());
  Result<std::int64_t> observed_at = in.i64();
  if (!observed_at.ok()) return observed_at.error();
  context.observed_at = observed_at.value();
  Result<std::uint8_t> precision = in.u8();
  if (!precision.ok()) return precision.error();
  if (precision.value() > static_cast<std::uint8_t>(Precision::Unknown)) {
    return Error(ErrorCode::ProtocolViolation, "publication context carries an invalid precision");
  }
  context.precision = static_cast<Precision>(precision.value());
  Result<std::uint8_t> cls = in.u8();
  if (!cls.ok()) return cls.error();
  if (cls.value() > static_cast<std::uint8_t>(EvidenceClass::Unknown)) {
    return Error(ErrorCode::ProtocolViolation,
                 "publication context carries an invalid evidence class");
  }
  context.evidence_class = static_cast<EvidenceClass>(cls.value());
  Result<std::uint8_t> provenance = in.u8();
  if (!provenance.ok()) return provenance.error();
  if (provenance.value() > static_cast<std::uint8_t>(Provenance::Unknown)) {
    return Error(ErrorCode::ProtocolViolation,
                 "publication context carries an invalid provenance");
  }
  context.provenance = static_cast<Provenance>(provenance.value());
  Result<std::uint64_t> evidence_generation = in.u64();
  if (!evidence_generation.ok()) return evidence_generation.error();
  context.evidence_generation = EvidenceGeneration(evidence_generation.value());
  const Status valid = context.validate();
  if (!valid.ok()) {
    return Error(ErrorCode::ProtocolViolation, "publication context is not valid",
                 valid.error().message());
  }
  return context;
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Invalid: return "INVALID";
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::Heartbeat: return "HEARTBEAT";
    case MessageType::HeartbeatAck: return "HEARTBEAT_ACK";
    case MessageType::RegisterPublisher: return "REGISTER_PUBLISHER";
    case MessageType::RegisterFederation: return "REGISTER_FEDERATION";
    case MessageType::RegisterSite: return "REGISTER_SITE";
    case MessageType::RegisterCluster: return "REGISTER_CLUSTER";
    case MessageType::RegisterAcceleratorClass: return "REGISTER_ACCELERATOR_CLASS";
    case MessageType::RegisterRuntime: return "REGISTER_RUNTIME";
    case MessageType::RegisterBackend: return "REGISTER_BACKEND";
    case MessageType::RegisterDomain: return "REGISTER_DOMAIN";
    case MessageType::RegisterPolicy: return "REGISTER_POLICY";
    case MessageType::RegisterArtifact: return "REGISTER_ARTIFACT";
    case MessageType::RegisterWorkloadClass: return "REGISTER_WORKLOAD_CLASS";
    case MessageType::RegisterWorkload: return "REGISTER_WORKLOAD";
    case MessageType::PublishCapability: return "PUBLISH_CAPABILITY";
    case MessageType::PublishCapacity: return "PUBLISH_CAPACITY";
    case MessageType::PublishPlacement: return "PUBLISH_PLACEMENT";
    case MessageType::PublishMigration: return "PUBLISH_MIGRATION";
    case MessageType::PublishMigrationStage: return "PUBLISH_MIGRATION_STAGE";
    case MessageType::PublishPortability: return "PUBLISH_PORTABILITY";
    case MessageType::RetireCluster: return "RETIRE_CLUSTER";
    case MessageType::FencePublisher: return "FENCE_PUBLISHER";
    case MessageType::QuerySnapshot: return "QUERY_SNAPSHOT";
    case MessageType::QueryPlacement: return "QUERY_PLACEMENT";
    case MessageType::QueryRejection: return "QUERY_REJECTION";
    case MessageType::QueryStrandedCapacity: return "QUERY_STRANDED_CAPACITY";
    case MessageType::QueryFragmentation: return "QUERY_FRAGMENTATION";
    case MessageType::QueryCompatibility: return "QUERY_COMPATIBILITY";
    case MessageType::QueryPortability: return "QUERY_PORTABILITY";
    case MessageType::QueryMigration: return "QUERY_MIGRATION";
    case MessageType::QueryMismatch: return "QUERY_MISMATCH";
    case MessageType::QueryDrift: return "QUERY_DRIFT";
    case MessageType::QueryHealth: return "QUERY_HEALTH";
    case MessageType::QueryBounds: return "QUERY_BOUNDS";
    case MessageType::Reply: return "REPLY";
    case MessageType::Failure: return "FAILURE";
    case MessageType::Shutdown: return "SHUTDOWN";
  }
  return "INVALID";
}

bool is_publication(MessageType type) noexcept {
  switch (type) {
    case MessageType::RegisterFederation:
    case MessageType::RegisterSite:
    case MessageType::RegisterCluster:
    case MessageType::RegisterAcceleratorClass:
    case MessageType::RegisterRuntime:
    case MessageType::RegisterBackend:
    case MessageType::RegisterDomain:
    case MessageType::RegisterPolicy:
    case MessageType::RegisterArtifact:
    case MessageType::RegisterWorkloadClass:
    case MessageType::RegisterWorkload:
    case MessageType::PublishCapability:
    case MessageType::PublishCapacity:
    case MessageType::PublishPlacement:
    case MessageType::PublishMigration:
    case MessageType::PublishMigrationStage:
    case MessageType::PublishPortability:
    case MessageType::RetireCluster:
      return true;
    default:
      return false;
  }
}

bool is_registration(MessageType type) noexcept {
  switch (type) {
    case MessageType::RegisterPublisher:
    case MessageType::RegisterFederation:
    case MessageType::RegisterSite:
    case MessageType::RegisterCluster:
    case MessageType::RegisterAcceleratorClass:
    case MessageType::RegisterRuntime:
    case MessageType::RegisterBackend:
    case MessageType::RegisterDomain:
    case MessageType::RegisterPolicy:
    case MessageType::RegisterArtifact:
    case MessageType::RegisterWorkloadClass:
    case MessageType::RegisterWorkload:
      return true;
    default:
      return false;
  }
}

bool is_query(MessageType type) noexcept {
  return type >= MessageType::QuerySnapshot && type <= MessageType::QueryBounds;
}

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Bounds& bounds) {
  if (!known_message_type(static_cast<std::uint16_t>(frame.type))) {
    return Error(ErrorCode::ProtocolViolation, "unknown message type in outgoing frame");
  }
  if (frame.payload.size() > bounds.max_frame_bytes) {
    return Error(ErrorCode::OversizedPayload, "outgoing frame payload exceeds the frame bound",
                 std::to_string(frame.payload.size()));
  }
  std::vector<std::uint8_t> image(kFrameHeaderSize + frame.payload.size(), 0);
  std::memcpy(image.data(), kProtocolMagic, sizeof(kProtocolMagic));
  write_u16(image.data() + 4, static_cast<std::uint16_t>(kProtocolVersion));
  write_u16(image.data() + 6, static_cast<std::uint16_t>(frame.type));
  image[8] = frame.flags;
  image[9] = 0;
  image[10] = 0;
  image[11] = 0;
  write_u64(image.data() + 12, frame.request_id);
  write_u32(image.data() + 20, static_cast<std::uint32_t>(frame.payload.size()));
  write_u32(image.data() + 24,
            crc32(frame.payload.data(), frame.payload.size()));
  if (!frame.payload.empty()) {
    std::memcpy(image.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
  }
  return image;
}

Result<Frame> decode_frame(const std::uint8_t* data, std::size_t length, const Bounds& bounds) {
  if (data == nullptr) {
    return Error(ErrorCode::ProtocolViolation, "frame buffer is null");
  }
  if (length < kFrameHeaderSize) {
    return Error(ErrorCode::ProtocolViolation, "frame is truncated before the header",
                 std::to_string(length));
  }
  if (std::memcmp(data, kProtocolMagic, sizeof(kProtocolMagic)) != 0) {
    return Error(ErrorCode::ProtocolViolation, "frame magic does not match");
  }
  const std::uint16_t version = read_u16(data + 4);
  if (version != static_cast<std::uint16_t>(kProtocolVersion)) {
    return Error(ErrorCode::UnsupportedVersion, "unsupported protocol version",
                 std::to_string(version));
  }
  const std::uint16_t raw_type = read_u16(data + 6);
  if (!known_message_type(raw_type)) {
    return Error(ErrorCode::ProtocolViolation, "unknown message type",
                 std::to_string(raw_type));
  }
  if (data[9] != 0 || data[10] != 0 || data[11] != 0) {
    return Error(ErrorCode::ProtocolViolation, "frame reserved bytes are not zero");
  }
  const std::uint32_t payload_length = read_u32(data + 20);
  if (payload_length > bounds.max_frame_bytes) {
    return Error(ErrorCode::OversizedPayload, "frame declares a payload beyond the frame bound",
                 std::to_string(payload_length));
  }
  if (length != kFrameHeaderSize + static_cast<std::size_t>(payload_length)) {
    return Error(ErrorCode::ProtocolViolation, "frame length does not match the declared payload",
                 "declared=" + std::to_string(payload_length) +
                     " available=" + std::to_string(length - kFrameHeaderSize));
  }
  const std::uint32_t expected_crc = read_u32(data + 24);
  const std::uint32_t actual_crc =
      crc32(data + kFrameHeaderSize, static_cast<std::size_t>(payload_length));
  if (expected_crc != actual_crc) {
    return Error(ErrorCode::IntegrityFailure, "frame payload checksum does not match");
  }
  Frame frame;
  frame.type = static_cast<MessageType>(raw_type);
  frame.flags = data[8];
  frame.request_id = read_u64(data + 12);
  frame.payload.assign(data + kFrameHeaderSize, data + kFrameHeaderSize + payload_length);
  return frame;
}

Result<Frame> FrameReader::next(int poll_millis) {
  if (socket_ == nullptr || bounds_ == nullptr) {
    return Error(ErrorCode::Internal, "frame reader is not attached to a socket");
  }
  if (buffer_.size() < kFrameHeaderSize) {
    buffer_.resize(kFrameHeaderSize);
  }
  while (buffered_ < kFrameHeaderSize) {
    const Result<std::size_t> read =
        socket_->read_some(buffer_.data() + buffered_, kFrameHeaderSize - buffered_, poll_millis);
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return Frame{};
    }
    buffered_ += read.value();
  }
  const std::size_t payload_length = read_u32(buffer_.data() + 20);
  if (payload_length > bounds_->max_frame_bytes) {
    return Error(ErrorCode::OversizedPayload, "frame declares a payload beyond the frame bound",
                 std::to_string(payload_length));
  }
  const std::size_t total = kFrameHeaderSize + payload_length;
  if (buffer_.size() < total) {
    buffer_.resize(total);
  }
  while (buffered_ < total) {
    const Result<std::size_t> read =
        socket_->read_some(buffer_.data() + buffered_, total - buffered_, poll_millis);
    if (!read.ok()) {
      return read.error();
    }
    if (read.value() == 0) {
      return Frame{};
    }
    buffered_ += read.value();
  }
  const Result<Frame> frame = decode_frame(buffer_.data(), total, *bounds_);
  buffered_ = 0;
  return frame;
}

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

void encode(Encoder& out, const HelloRequest& value) {
  out.u16(value.protocol);
  out.str(value.client_version);
  out.str(value.publisher.value());
}

Result<HelloRequest> decode_hello(Decoder& in) {
  HelloRequest value;
  Result<std::uint16_t> protocol = in.u16();
  if (!protocol.ok()) return protocol.error();
  value.protocol = protocol.value();
  Result<std::string> version = in.str();
  if (!version.ok()) return version.error();
  if (version.value().size() > kMaxBannerLength) {
    return Error(ErrorCode::BoundExceeded, "client version banner is too long");
  }
  value.client_version = version.take();
  Result<std::string> publisher = in.str();
  if (!publisher.ok()) return publisher.error();
  if (!publisher.value().empty() && !is_valid_identifier(publisher.value())) {
    return Error(ErrorCode::ProtocolViolation, "HELLO carries a malformed publisher identifier");
  }
  value.publisher = PublisherId::unchecked(publisher.take());
  if (value.protocol != kProtocolVersion) {
    return Error(ErrorCode::UnsupportedVersion, "unsupported protocol version",
                 std::to_string(value.protocol));
  }
  return value;
}

void encode(Encoder& out, const HelloReply& value) {
  out.u16(value.protocol);
  out.str(value.server_version);
  out.u64(value.epoch.value());
  out.str(value.coordinator_id);
}

Result<HelloReply> decode_hello_reply(Decoder& in) {
  HelloReply value;
  Result<std::uint16_t> protocol = in.u16();
  if (!protocol.ok()) return protocol.error();
  value.protocol = protocol.value();
  Result<std::string> version = in.str();
  if (!version.ok()) return version.error();
  value.server_version = version.take();
  Result<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return epoch.error();
  value.epoch = CoordinatorEpoch(epoch.value());
  Result<std::string> id = in.str();
  if (!id.ok()) return id.error();
  value.coordinator_id = id.take();
  return value;
}

void encode(Encoder& out, const RegisterPublisherRequest& value) {
  encode_publication_context(out, value.context);
  out.str(value.role);
}

Result<RegisterPublisherRequest> decode_register_publisher(Decoder& in) {
  RegisterPublisherRequest value;
  Result<PublicationContext> context = decode_publication_context(in);
  if (!context.ok()) return context.error();
  value.context = context.take();
  Result<std::string> role = in.str();
  if (!role.ok()) return role.error();
  value.role = role.take();
  return value;
}

void encode(Encoder& out, const PublicationAck& value) {
  out.u8(static_cast<std::uint8_t>(value.disposition));
  out.u8(static_cast<std::uint8_t>(value.code));
  out.str(value.detail);
  out.u64(value.watermark.value());
  out.u64(value.sequence_gap);
}

Result<PublicationAck> decode_publication_ack(Decoder& in) {
  PublicationAck value;
  Result<std::uint8_t> disposition = in.u8();
  if (!disposition.ok()) return disposition.error();
  if (disposition.value() > static_cast<std::uint8_t>(IngestDisposition::Deferred)) {
    return Error(ErrorCode::ProtocolViolation, "reply carries an invalid ingest disposition");
  }
  value.disposition = static_cast<IngestDisposition>(disposition.value());
  Result<std::uint8_t> code = in.u8();
  if (!code.ok()) return code.error();
  if (code.value() > static_cast<std::uint8_t>(ErrorCode::Internal)) {
    return Error(ErrorCode::ProtocolViolation, "reply carries an invalid error code");
  }
  value.code = static_cast<ErrorCode>(code.value());
  Result<std::string> detail = in.str();
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  Result<std::uint64_t> watermark = in.u64();
  if (!watermark.ok()) return watermark.error();
  value.watermark = Sequence(watermark.value());
  Result<std::uint64_t> gap = in.u64();
  if (!gap.ok()) return gap.error();
  value.sequence_gap = gap.value();
  return value;
}

void encode(Encoder& out, const ReplyPayload& value) {
  out.u8(static_cast<std::uint8_t>(value.code));
  out.str(value.message);
  // Reply text and detail are deterministic renderings and are legitimately larger than
  // the small per-string field bound; they use an explicitly bounded block field.
  out.text_block(value.detail);
  out.str(value.digest);
  out.text_block(value.text);
  out.u32(static_cast<std::uint32_t>(value.rows.size()));
  for (const WireRow& row : value.rows) {
    out.u32(static_cast<std::uint32_t>(row.fields.size()));
    for (const WireField& field : row.fields) {
      out.str(field.name);
      out.str(field.value);
    }
  }
}

Result<ReplyPayload> decode_reply(Decoder& in) {
  ReplyPayload value;
  Result<std::uint8_t> code = in.u8();
  if (!code.ok()) return code.error();
  if (code.value() > static_cast<std::uint8_t>(ErrorCode::Internal)) {
    return Error(ErrorCode::ProtocolViolation, "reply carries an invalid error code");
  }
  value.code = static_cast<ErrorCode>(code.value());
  Result<std::string> message = in.str();
  if (!message.ok()) return message.error();
  value.message = message.take();
  Result<std::string> detail = in.text_block();
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  Result<std::string> digest = in.str();
  if (!digest.ok()) return digest.error();
  value.digest = digest.take();
  Result<std::string> text = in.text_block();
  if (!text.ok()) return text.error();
  value.text = text.take();
  Result<std::uint32_t> row_count = in.u32();
  if (!row_count.ok()) return row_count.error();
  if (row_count.value() > 65536) {
    return Error(ErrorCode::BoundExceeded, "reply carries too many rows");
  }
  value.rows.reserve(row_count.value());
  for (std::uint32_t i = 0; i < row_count.value(); ++i) {
    Result<std::uint32_t> field_count = in.u32();
    if (!field_count.ok()) return field_count.error();
    if (field_count.value() > 64) {
      return Error(ErrorCode::BoundExceeded, "reply row carries too many fields");
    }
    WireRow row;
    row.fields.reserve(field_count.value());
    for (std::uint32_t f = 0; f < field_count.value(); ++f) {
      WireField field;
      Result<std::string> name = in.str();
      if (!name.ok()) return name.error();
      field.name = name.take();
      Result<std::string> field_value = in.str();
      if (!field_value.ok()) return field_value.error();
      field.value = field_value.take();
      row.fields.push_back(std::move(field));
    }
    value.rows.push_back(std::move(row));
  }
  return value;
}

void encode(Encoder& out, const FenceRequest& value) {
  out.str(value.publisher.value());
  out.u64(value.boot.value());
  out.str(value.reason);
  out.str(value.token);
}

Result<FenceRequest> decode_fence(Decoder& in) {
  FenceRequest value;
  Result<std::string> publisher = in.str();
  if (!publisher.ok()) return publisher.error();
  if (!publisher.value().empty() && !is_valid_identifier(publisher.value())) {
    return Error(ErrorCode::ProtocolViolation, "FENCE carries a malformed publisher identifier");
  }
  value.publisher = PublisherId::unchecked(publisher.take());
  Result<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return boot.error();
  value.boot = BootGeneration(boot.value());
  Result<std::string> reason = in.str();
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  Result<std::string> token = in.str();
  if (!token.ok()) return token.error();
  value.token = token.take();
  return value;
}

void encode(Encoder& out, const RetireClusterRequest& value) {
  encode_publication_context(out, value.context);
  out.str(value.cluster.value());
  out.u64(value.generation.value());
  out.str(value.reason);
}

Result<RetireClusterRequest> decode_retire_cluster(Decoder& in) {
  RetireClusterRequest value;
  Result<PublicationContext> context = decode_publication_context(in);
  if (!context.ok()) return context.error();
  value.context = context.take();
  Result<std::string> cluster = in.str();
  if (!cluster.ok()) return cluster.error();
  if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
    return Error(ErrorCode::ProtocolViolation, "RETIRE_CLUSTER carries a malformed cluster id");
  }
  value.cluster = ClusterId::unchecked(cluster.take());
  Result<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return generation.error();
  value.generation = ClusterGeneration(generation.value());
  Result<std::string> reason = in.str();
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  return value;
}

void encode(Encoder& out, const MigrationStageRequest& value) {
  encode_publication_context(out, value.context);
  out.str(value.migration.value());
  out.u64(value.generation.value());
  out.u8(static_cast<std::uint8_t>(value.stage));
  out.str(value.detail);
}

Result<MigrationStageRequest> decode_migration_stage(Decoder& in) {
  MigrationStageRequest value;
  Result<PublicationContext> context = decode_publication_context(in);
  if (!context.ok()) return context.error();
  value.context = context.take();
  Result<std::string> migration = in.str();
  if (!migration.ok()) return migration.error();
  if (migration.value().empty() || !is_valid_identifier(migration.value())) {
    return Error(ErrorCode::ProtocolViolation, "migration stage carries a malformed migration id");
  }
  value.migration = MigrationId::unchecked(migration.take());
  Result<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return generation.error();
  value.generation = MigrationGeneration(generation.value());
  Result<std::uint8_t> stage = in.u8();
  if (!stage.ok()) return stage.error();
  if (stage.value() > static_cast<std::uint8_t>(MigrationStage::OutcomeUnknown)) {
    return Error(ErrorCode::ProtocolViolation, "migration stage event carries an invalid stage");
  }
  value.stage = static_cast<MigrationStage>(stage.value());
  Result<std::string> detail = in.str();
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  return value;
}

void encode(Encoder& out, const CapabilityPublicationRequest& value) {
  encode_publication_context(out, value.context);
  out.str(value.cluster.value());
  out.u64(value.cluster_generation.value());
  out.u64(value.capability_generation.value());
  codec::encode_capability_set(out, value.capabilities);
}

Result<CapabilityPublicationRequest> decode_capability_publication(Decoder& in) {
  CapabilityPublicationRequest value;
  Result<PublicationContext> context = decode_publication_context(in);
  if (!context.ok()) return context.error();
  value.context = context.take();
  Result<std::string> cluster = in.str();
  if (!cluster.ok()) return cluster.error();
  if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
    return Error(ErrorCode::ProtocolViolation, "capability publication carries a malformed cluster");
  }
  value.cluster = ClusterId::unchecked(cluster.take());
  Result<std::uint64_t> cluster_generation = in.u64();
  if (!cluster_generation.ok()) return cluster_generation.error();
  value.cluster_generation = ClusterGeneration(cluster_generation.value());
  Result<std::uint64_t> capability_generation = in.u64();
  if (!capability_generation.ok()) return capability_generation.error();
  value.capability_generation = AcceleratorCapabilityGeneration(capability_generation.value());
  Result<CapabilitySet> capabilities = codec::decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  return value;
}

void encode(Encoder& out, const CapacityPublicationRequest& value) {
  encode_publication_context(out, value.context);
  out.str(value.cluster.value());
  out.u64(value.cluster_generation.value());
  out.u64(value.capacity_generation.value());
  out.u32(static_cast<std::uint32_t>(value.pools.size()));
  for (const CapacityPool& pool : value.pools) {
    codec::encode_capacity_pool(out, pool);
  }
}

Result<CapacityPublicationRequest> decode_capacity_publication(Decoder& in) {
  CapacityPublicationRequest value;
  Result<PublicationContext> context = decode_publication_context(in);
  if (!context.ok()) return context.error();
  value.context = context.take();
  Result<std::string> cluster = in.str();
  if (!cluster.ok()) return cluster.error();
  if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
    return Error(ErrorCode::ProtocolViolation, "capacity publication carries a malformed cluster");
  }
  value.cluster = ClusterId::unchecked(cluster.take());
  Result<std::uint64_t> cluster_generation = in.u64();
  if (!cluster_generation.ok()) return cluster_generation.error();
  value.cluster_generation = ClusterGeneration(cluster_generation.value());
  Result<std::uint64_t> capacity_generation = in.u64();
  if (!capacity_generation.ok()) return capacity_generation.error();
  value.capacity_generation = CapacityGeneration(capacity_generation.value());
  Result<std::uint32_t> pool_count = in.u32();
  if (!pool_count.ok()) return pool_count.error();
  if (pool_count.value() > bounds::kMaxPoolsPerCluster) {
    return Error(ErrorCode::BoundExceeded, "capacity publication declares too many pools");
  }
  value.pools.reserve(pool_count.value());
  for (std::uint32_t i = 0; i < pool_count.value(); ++i) {
    Result<CapacityPool> pool = codec::decode_capacity_pool(in);
    if (!pool.ok()) return pool.error();
    value.pools.push_back(pool.take());
  }
  return value;
}

void encode(Encoder& out, const SnapshotQuery& value) {
  out.str(value.federation.value());
  out.u64(value.max_records);
  out.boolean(value.include_history);
}

Result<SnapshotQuery> decode_snapshot_query(Decoder& in) {
  SnapshotQuery value;
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "snapshot query carries a malformed federation");
  }
  value.federation = FederationId::unchecked(federation.take());
  Result<std::uint64_t> max_records = in.u64();
  if (!max_records.ok()) return max_records.error();
  value.max_records = max_records.value();
  Result<bool> include_history = in.boolean();
  if (!include_history.ok()) return include_history.error();
  value.include_history = include_history.value();
  return value;
}

void encode(Encoder& out, const PlacementQuery& value) {
  out.str(value.placement.value());
  out.str(value.workload.value());
  out.boolean(value.latest_by_workload);
}

Result<PlacementQuery> decode_placement_query(Decoder& in) {
  PlacementQuery value;
  Result<std::string> placement = in.str();
  if (!placement.ok()) return placement.error();
  if (!placement.value().empty() && !is_valid_identifier(placement.value())) {
    return Error(ErrorCode::ProtocolViolation, "placement query carries a malformed placement id");
  }
  value.placement = PlacementId::unchecked(placement.take());
  Result<std::string> workload = in.str();
  if (!workload.ok()) return workload.error();
  if (!workload.value().empty() && !is_valid_identifier(workload.value())) {
    return Error(ErrorCode::ProtocolViolation, "placement query carries a malformed workload id");
  }
  value.workload = WorkloadId::unchecked(workload.take());
  Result<bool> latest = in.boolean();
  if (!latest.ok()) return latest.error();
  value.latest_by_workload = latest.value();
  return value;
}

void encode(Encoder& out, const RejectionQuery& value) {
  out.str(value.placement.value());
  out.str(value.candidate.value());
}

Result<RejectionQuery> decode_rejection_query(Decoder& in) {
  RejectionQuery value;
  Result<std::string> placement = in.str();
  if (!placement.ok()) return placement.error();
  if (placement.value().empty() || !is_valid_identifier(placement.value())) {
    return Error(ErrorCode::ProtocolViolation, "rejection query carries a malformed placement id");
  }
  value.placement = PlacementId::unchecked(placement.take());
  Result<std::string> candidate = in.str();
  if (!candidate.ok()) return candidate.error();
  if (candidate.value().empty() || !is_valid_identifier(candidate.value())) {
    return Error(ErrorCode::ProtocolViolation, "rejection query carries a malformed cluster id");
  }
  value.candidate = ClusterId::unchecked(candidate.take());
  return value;
}

void encode(Encoder& out, const StrandedQuery& value) {
  out.str(value.request.federation.value());
  out.str(value.request.workload_class.value());
  out.u8(static_cast<std::uint8_t>(value.request.kind));
  out.str(value.request.cluster.value());
  out.str(value.request.accelerator_class.value());
  out.boolean(value.request.include_stale);
}

Result<StrandedQuery> decode_stranded_query(Decoder& in) {
  StrandedQuery value;
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "stranded query carries a malformed federation");
  }
  value.request.federation = FederationId::unchecked(federation.take());
  Result<std::string> workload_class = in.str();
  if (!workload_class.ok()) return workload_class.error();
  if (!workload_class.value().empty() && !is_valid_identifier(workload_class.value())) {
    return Error(ErrorCode::ProtocolViolation, "stranded query carries a malformed workload class");
  }
  value.request.workload_class = WorkloadClassId::unchecked(workload_class.take());
  Result<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return kind.error();
  if (kind.value() > static_cast<std::uint8_t>(ResourceKind::Custom)) {
    return Error(ErrorCode::ProtocolViolation, "stranded query carries an invalid resource kind");
  }
  value.request.kind = static_cast<ResourceKind>(kind.value());
  Result<std::string> cluster = in.str();
  if (!cluster.ok()) return cluster.error();
  if (!cluster.value().empty() && !is_valid_identifier(cluster.value())) {
    return Error(ErrorCode::ProtocolViolation, "stranded query carries a malformed cluster");
  }
  value.request.cluster = ClusterId::unchecked(cluster.take());
  Result<std::string> accelerator_class = in.str();
  if (!accelerator_class.ok()) return accelerator_class.error();
  if (!accelerator_class.value().empty() && !is_valid_identifier(accelerator_class.value())) {
    return Error(ErrorCode::ProtocolViolation,
                 "stranded query carries a malformed accelerator class");
  }
  value.request.accelerator_class = AcceleratorClassId::unchecked(accelerator_class.take());
  Result<bool> include_stale = in.boolean();
  if (!include_stale.ok()) return include_stale.error();
  value.request.include_stale = include_stale.value();
  return value;
}

void encode(Encoder& out, const FragmentationQuery& value) {
  out.str(value.federation.value());
  out.str(value.workload_class.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
}

Result<FragmentationQuery> decode_fragmentation_query(Decoder& in) {
  FragmentationQuery value;
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "fragmentation query carries a malformed federation");
  }
  value.federation = FederationId::unchecked(federation.take());
  Result<std::string> workload_class = in.str();
  if (!workload_class.ok()) return workload_class.error();
  if (workload_class.value().empty() || !is_valid_identifier(workload_class.value())) {
    return Error(ErrorCode::ProtocolViolation,
                 "fragmentation query carries a malformed workload class");
  }
  value.workload_class = WorkloadClassId::unchecked(workload_class.take());
  Result<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return kind.error();
  if (kind.value() > static_cast<std::uint8_t>(ResourceKind::Custom)) {
    return Error(ErrorCode::ProtocolViolation,
                 "fragmentation query carries an invalid resource kind");
  }
  value.kind = static_cast<ResourceKind>(kind.value());
  return value;
}

void encode(Encoder& out, const CompatibilityQuery& value) {
  out.str(value.cluster.value());
  out.str(value.workload.value());
}

Result<CompatibilityQuery> decode_compatibility_query(Decoder& in) {
  CompatibilityQuery value;
  Result<std::string> cluster = in.str();
  if (!cluster.ok()) return cluster.error();
  if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
    return Error(ErrorCode::ProtocolViolation, "compatibility query carries a malformed cluster");
  }
  value.cluster = ClusterId::unchecked(cluster.take());
  Result<std::string> workload = in.str();
  if (!workload.ok()) return workload.error();
  if (workload.value().empty() || !is_valid_identifier(workload.value())) {
    return Error(ErrorCode::ProtocolViolation, "compatibility query carries a malformed workload");
  }
  value.workload = WorkloadId::unchecked(workload.take());
  return value;
}

void encode(Encoder& out, const PortabilityQuery& value) {
  out.str(value.workload.value());
  out.str(value.destination.value());
  out.boolean(value.evaluate);
}

Result<PortabilityQuery> decode_portability_query(Decoder& in) {
  PortabilityQuery value;
  Result<std::string> workload = in.str();
  if (!workload.ok()) return workload.error();
  if (workload.value().empty() || !is_valid_identifier(workload.value())) {
    return Error(ErrorCode::ProtocolViolation, "portability query carries a malformed workload");
  }
  value.workload = WorkloadId::unchecked(workload.take());
  Result<std::string> destination = in.str();
  if (!destination.ok()) return destination.error();
  if (destination.value().empty() || !is_valid_identifier(destination.value())) {
    return Error(ErrorCode::ProtocolViolation, "portability query carries a malformed destination");
  }
  value.destination = ClusterId::unchecked(destination.take());
  Result<bool> evaluate = in.boolean();
  if (!evaluate.ok()) return evaluate.error();
  value.evaluate = evaluate.value();
  return value;
}

void encode(Encoder& out, const MigrationQuery& value) { out.str(value.migration.value()); }

Result<MigrationQuery> decode_migration_query(Decoder& in) {
  MigrationQuery value;
  Result<std::string> migration = in.str();
  if (!migration.ok()) return migration.error();
  if (migration.value().empty() || !is_valid_identifier(migration.value())) {
    return Error(ErrorCode::ProtocolViolation, "migration query carries a malformed migration id");
  }
  value.migration = MigrationId::unchecked(migration.take());
  return value;
}

void encode(Encoder& out, const MismatchQuery& value) {
  out.str(value.request.federation.value());
  out.u16(static_cast<std::uint16_t>(value.request.capability_key.key));
  out.str(value.request.capability_key.custom.value());
  out.i64(value.request.window.from);
  out.i64(value.request.window.to);
  out.u8(static_cast<std::uint8_t>(value.request.kind));
  out.boolean(value.request.include_stale);
}

Result<MismatchQuery> decode_mismatch_query(Decoder& in) {
  MismatchQuery value;
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "mismatch query carries a malformed federation");
  }
  value.request.federation = FederationId::unchecked(federation.take());
  Result<std::uint16_t> key = in.u16();
  if (!key.ok()) return key.error();
  Result<std::string> custom = in.str();
  if (!custom.ok()) return custom.error();
  if (key.value() > static_cast<std::uint16_t>(CapabilityKey::Custom)) {
    return Error(ErrorCode::ProtocolViolation, "mismatch query carries an invalid capability key");
  }
  value.request.capability_key.key = static_cast<CapabilityKey>(key.value());
  if (!custom.value().empty()) {
    if (!is_valid_identifier(custom.value())) {
      return Error(ErrorCode::ProtocolViolation,
                   "mismatch query carries a malformed custom capability key");
    }
    value.request.capability_key.custom = CapabilityKeyId::unchecked(custom.take());
  }
  Result<std::int64_t> from = in.i64();
  if (!from.ok()) return from.error();
  value.request.window.from = from.value();
  Result<std::int64_t> to = in.i64();
  if (!to.ok()) return to.error();
  value.request.window.to = to.value();
  if (value.request.window.from != 0 && value.request.window.to != 0 &&
      value.request.window.to < value.request.window.from) {
    return Error(ErrorCode::ProtocolViolation, "mismatch query window is inverted");
  }
  Result<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return kind.error();
  if (kind.value() > static_cast<std::uint8_t>(ResourceKind::Custom)) {
    return Error(ErrorCode::ProtocolViolation, "mismatch query carries an invalid resource kind");
  }
  value.request.kind = static_cast<ResourceKind>(kind.value());
  Result<bool> include_stale = in.boolean();
  if (!include_stale.ok()) return include_stale.error();
  value.request.include_stale = include_stale.value();
  return value;
}

void encode(Encoder& out, const DriftQuery& value) {
  out.str(value.federation.value());
  out.boolean(value.have_intended);
  out.str(value.intended.federation.value());
  out.u64(value.intended.generation.value());
  out.u32(static_cast<std::uint32_t>(value.intended.expected_sites.size()));
  for (const SiteId& site : value.intended.expected_sites) {
    out.str(site.value());
  }
  out.u32(static_cast<std::uint32_t>(value.intended.expected_clusters.size()));
  for (const ClusterId& cluster : value.intended.expected_clusters) {
    out.str(cluster.value());
  }
  out.u32(static_cast<std::uint32_t>(value.intended.expected_capabilities.size()));
  for (const IntendedFederationState::ExpectedCapability& capability :
       value.intended.expected_capabilities) {
    out.str(capability.cluster.value());
    codec::encode_capability_ref(out, capability.key);
    codec::encode_capability_value(out, capability.value);
    out.u8(static_cast<std::uint8_t>(capability.comparator));
  }
  out.u32(static_cast<std::uint32_t>(value.intended.expected_runtime_versions.size()));
  for (const auto& entry : value.intended.expected_runtime_versions) {
    out.str(entry.first.value());
    out.str(entry.second);
  }
  out.u32(static_cast<std::uint32_t>(value.intended.expected_capacity.size()));
  for (const auto& entry : value.intended.expected_capacity) {
    out.str(entry.first.value());
    codec::encode_ledger(out, entry.second);
  }
  out.u64(value.intended.expected_policy_generation.value());
  out.u8(static_cast<std::uint8_t>(value.intended.precision));
  out.u8(static_cast<std::uint8_t>(value.intended.evidence_class));
  out.boolean(value.behavior_window_supplied);
  out.i64(value.before_window.from);
  out.i64(value.before_window.to);
  out.i64(value.after_window.from);
  out.i64(value.after_window.to);
}

Result<DriftQuery> decode_drift_query(Decoder& in) {
  DriftQuery value;
  Result<std::string> federation = in.str();
  if (!federation.ok()) return federation.error();
  if (!federation.value().empty() && !is_valid_identifier(federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed federation");
  }
  value.federation = FederationId::unchecked(federation.take());
  Result<bool> have_intended = in.boolean();
  if (!have_intended.ok()) return have_intended.error();
  value.have_intended = have_intended.value();
  Result<std::string> intended_federation = in.str();
  if (!intended_federation.ok()) return intended_federation.error();
  if (!intended_federation.value().empty() && !is_valid_identifier(intended_federation.value())) {
    return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed intended federation");
  }
  value.intended.federation = FederationId::unchecked(intended_federation.take());
  Result<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return generation.error();
  value.intended.generation = FederationGeneration(generation.value());

  {
    Result<std::uint32_t> count = in.u32();
    if (!count.ok()) return count.error();
    if (count.value() > 65536) {
      return Error(ErrorCode::BoundExceeded, "drift query declares too many site ids");
    }
    value.intended.expected_sites.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
      Result<std::string> text = in.str();
      if (!text.ok()) return text.error();
      if (text.value().empty() || !is_valid_identifier(text.value())) {
        return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed site id");
      }
      value.intended.expected_sites.push_back(SiteId::unchecked(text.take()));
    }
  }
  {
    Result<std::uint32_t> count = in.u32();
    if (!count.ok()) return count.error();
    if (count.value() > 65536) {
      return Error(ErrorCode::BoundExceeded, "drift query declares too many cluster ids");
    }
    value.intended.expected_clusters.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
      Result<std::string> text = in.str();
      if (!text.ok()) return text.error();
      if (text.value().empty() || !is_valid_identifier(text.value())) {
        return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed cluster id");
      }
      value.intended.expected_clusters.push_back(ClusterId::unchecked(text.take()));
    }
  }

  Result<std::uint32_t> capability_count = in.u32();
  if (!capability_count.ok()) return capability_count.error();
  if (capability_count.value() > 4096) {
    return Error(ErrorCode::BoundExceeded, "drift query declares too many expected capabilities");
  }
  for (std::uint32_t i = 0; i < capability_count.value(); ++i) {
    IntendedFederationState::ExpectedCapability capability;
    Result<std::string> cluster = in.str();
    if (!cluster.ok()) return cluster.error();
    if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
      return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed cluster id");
    }
    capability.cluster = ClusterId::unchecked(cluster.take());
    Result<CapabilityRef> key = codec::decode_capability_ref(in);
    if (!key.ok()) return key.error();
    capability.key = key.take();
    Result<CapabilityValue> cap_value = codec::decode_capability_value(in);
    if (!cap_value.ok()) return cap_value.error();
    capability.value = cap_value.take();
    Result<std::uint8_t> comparator = in.u8();
    if (!comparator.ok()) return comparator.error();
    if (comparator.value() > static_cast<std::uint8_t>(CapabilityComparator::AllOf)) {
      return Error(ErrorCode::ProtocolViolation,
                   "drift query carries an invalid capability comparator");
    }
    capability.comparator = static_cast<CapabilityComparator>(comparator.value());
    value.intended.expected_capabilities.push_back(std::move(capability));
  }

  Result<std::uint32_t> runtime_count = in.u32();
  if (!runtime_count.ok()) return runtime_count.error();
  if (runtime_count.value() > 65536) {
    return Error(ErrorCode::BoundExceeded, "drift query declares too many runtime expectations");
  }
  for (std::uint32_t i = 0; i < runtime_count.value(); ++i) {
    Result<std::string> cluster = in.str();
    if (!cluster.ok()) return cluster.error();
    if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
      return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed cluster id");
    }
    Result<std::string> version = in.str();
    if (!version.ok()) return version.error();
    value.intended.expected_runtime_versions.emplace_back(ClusterId::unchecked(cluster.take()),
                                                          version.take());
  }

  Result<std::uint32_t> capacity_count = in.u32();
  if (!capacity_count.ok()) return capacity_count.error();
  if (capacity_count.value() > 65536) {
    return Error(ErrorCode::BoundExceeded, "drift query declares too many capacity expectations");
  }
  for (std::uint32_t i = 0; i < capacity_count.value(); ++i) {
    Result<std::string> cluster = in.str();
    if (!cluster.ok()) return cluster.error();
    if (cluster.value().empty() || !is_valid_identifier(cluster.value())) {
      return Error(ErrorCode::ProtocolViolation, "drift query carries a malformed cluster id");
    }
    const ClusterId id = ClusterId::unchecked(cluster.take());
    Result<CapacityLedger> ledger = codec::decode_ledger(in);
    if (!ledger.ok()) return ledger.error();
    value.intended.expected_capacity.emplace_back(id, ledger.value());
  }

  Result<std::uint64_t> policy_generation = in.u64();
  if (!policy_generation.ok()) return policy_generation.error();
  value.intended.expected_policy_generation = PolicyGeneration(policy_generation.value());
  Result<std::uint8_t> precision = in.u8();
  if (!precision.ok()) return precision.error();
  if (precision.value() > static_cast<std::uint8_t>(Precision::Unknown)) {
    return Error(ErrorCode::ProtocolViolation, "drift query carries an invalid precision");
  }
  value.intended.precision = static_cast<Precision>(precision.value());
  Result<std::uint8_t> cls = in.u8();
  if (!cls.ok()) return cls.error();
  if (cls.value() > static_cast<std::uint8_t>(EvidenceClass::Unknown)) {
    return Error(ErrorCode::ProtocolViolation, "drift query carries an invalid evidence class");
  }
  value.intended.evidence_class = static_cast<EvidenceClass>(cls.value());
  Result<bool> behavior = in.boolean();
  if (!behavior.ok()) return behavior.error();
  value.behavior_window_supplied = behavior.value();
  Result<std::int64_t> from = in.i64();
  if (!from.ok()) return from.error();
  value.before_window.from = from.value();
  Result<std::int64_t> to = in.i64();
  if (!to.ok()) return to.error();
  value.before_window.to = to.value();
  Result<std::int64_t> after_from = in.i64();
  if (!after_from.ok()) return after_from.error();
  value.after_window.from = after_from.value();
  Result<std::int64_t> after_to = in.i64();
  if (!after_to.ok()) return after_to.error();
  value.after_window.to = after_to.value();

  if (value.have_intended) {
    const Status valid = value.intended.validate();
    if (!valid.ok()) {
      return Error(ErrorCode::ProtocolViolation, "drift query intended state is not valid",
                   valid.error().message());
    }
  }
  return value;
}

#define FO_ENCODE_PUBLICATION_BOILERPLATE(RecordType, EncodeFn)                 \
  void encode_publication(Encoder& out, const PublicationContext& ctx,          \
                          const RecordType& record) {                           \
    encode_publication_context(out, ctx);                                       \
    codec::EncodeFn(out, record);                                               \
  }

FO_ENCODE_PUBLICATION_BOILERPLATE(FederationRecord, encode_federation)
FO_ENCODE_PUBLICATION_BOILERPLATE(SiteRecord, encode_site)
FO_ENCODE_PUBLICATION_BOILERPLATE(ClusterRecord, encode_cluster)
FO_ENCODE_PUBLICATION_BOILERPLATE(AcceleratorClassRecord, encode_accelerator_class)
FO_ENCODE_PUBLICATION_BOILERPLATE(RuntimeRecord, encode_runtime)
FO_ENCODE_PUBLICATION_BOILERPLATE(BackendRecord, encode_backend)
FO_ENCODE_PUBLICATION_BOILERPLATE(DomainRecord, encode_domain)
FO_ENCODE_PUBLICATION_BOILERPLATE(PolicyRecord, encode_policy)
FO_ENCODE_PUBLICATION_BOILERPLATE(ArtifactRecord, encode_artifact)
FO_ENCODE_PUBLICATION_BOILERPLATE(WorkloadClassRecord, encode_workload_class)
FO_ENCODE_PUBLICATION_BOILERPLATE(WorkloadRecord, encode_workload)
FO_ENCODE_PUBLICATION_BOILERPLATE(PlacementRecord, encode_placement)
FO_ENCODE_PUBLICATION_BOILERPLATE(MigrationRecord, encode_migration)
FO_ENCODE_PUBLICATION_BOILERPLATE(PortabilityAssessment, encode_portability)

#undef FO_ENCODE_PUBLICATION_BOILERPLATE

#define FO_DECODE_PUBLICATION_BOILERPLATE(RecordType, DecodeFn)                       \
  Result<RecordType> DecodeFn##_publication(Decoder& in, PublicationContext& ctx) {   \
    Result<PublicationContext> context = decode_publication_context(in);              \
    if (!context.ok()) {                                                              \
      return context.error();                                                         \
    }                                                                                 \
    ctx = context.take();                                                             \
    Result<RecordType> record = codec::DecodeFn(in);                                  \
    if (!record.ok()) {                                                               \
      return record.error();                                                          \
    }                                                                                 \
    return record.take();                                                             \
  }

FO_DECODE_PUBLICATION_BOILERPLATE(FederationRecord, decode_federation)
FO_DECODE_PUBLICATION_BOILERPLATE(SiteRecord, decode_site)
FO_DECODE_PUBLICATION_BOILERPLATE(ClusterRecord, decode_cluster)
FO_DECODE_PUBLICATION_BOILERPLATE(AcceleratorClassRecord, decode_accelerator_class)
FO_DECODE_PUBLICATION_BOILERPLATE(RuntimeRecord, decode_runtime)
FO_DECODE_PUBLICATION_BOILERPLATE(BackendRecord, decode_backend)
FO_DECODE_PUBLICATION_BOILERPLATE(DomainRecord, decode_domain)
FO_DECODE_PUBLICATION_BOILERPLATE(PolicyRecord, decode_policy)
FO_DECODE_PUBLICATION_BOILERPLATE(ArtifactRecord, decode_artifact)
FO_DECODE_PUBLICATION_BOILERPLATE(WorkloadClassRecord, decode_workload_class)
FO_DECODE_PUBLICATION_BOILERPLATE(WorkloadRecord, decode_workload)
FO_DECODE_PUBLICATION_BOILERPLATE(PlacementRecord, decode_placement)
FO_DECODE_PUBLICATION_BOILERPLATE(MigrationRecord, decode_migration)
FO_DECODE_PUBLICATION_BOILERPLATE(PortabilityAssessment, decode_portability)

#undef FO_DECODE_PUBLICATION_BOILERPLATE

}  // namespace fo::protocol
