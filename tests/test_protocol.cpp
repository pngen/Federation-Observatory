// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Framing, payload codecs, and the adversarial protocol surface. Every rejection must
// happen before any payload field is interpreted.

#include <cstring>
#include <string>
#include <vector>

#include "federation_observatory/protocol.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

std::vector<std::uint8_t> frame_bytes(fo::protocol::MessageType type, std::uint8_t flags,
                                      std::uint64_t request_id,
                                      const std::vector<std::uint8_t>& payload) {
  fo::protocol::Frame frame;
  frame.type = type;
  frame.flags = flags;
  frame.request_id = request_id;
  frame.payload = payload;
  return FO_UNWRAP(fo::protocol::encode_frame(frame, fo::default_bounds()));
}

void put_u16(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint16_t value) {
  buffer[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    buffer[offset + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

}  // namespace

FO_TEST(protocol, frame_round_trip) {
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
  const std::vector<std::uint8_t> image = frame_bytes(fo::protocol::MessageType::Hello,
                                                      fo::protocol::frame_flag(fo::protocol::FrameFlags::Request),
                                                      42, payload);
  FO_CHECK_EQ(image.size(), fo::protocol::kFrameHeaderSize + payload.size());
  const fo::protocol::Frame decoded =
      FO_UNWRAP(fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds()));
  FO_CHECK(decoded.type == fo::protocol::MessageType::Hello);
  FO_CHECK_EQ(decoded.request_id, 42u);
  FO_CHECK(decoded.is_request());
  FO_CHECK_EQ(decoded.payload.size(), payload.size());
  FO_CHECK(std::memcmp(decoded.payload.data(), payload.data(), payload.size()) == 0);
}

FO_TEST(protocol, empty_frame_is_rejected) {
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(nullptr, 0, fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::ProtocolViolation);
}

FO_TEST(protocol, truncated_header_is_rejected) {
  const std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  for (std::size_t length = 1; length < fo::protocol::kFrameHeaderSize; ++length) {
    const fo::Result<fo::protocol::Frame> result =
        fo::protocol::decode_frame(image.data(), length, fo::default_bounds());
    FO_CHECK(!result.ok());
  }
}

FO_TEST(protocol, truncated_payload_is_rejected) {
  const std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Hello, 0, 1, {1, 2, 3, 4, 5, 6, 7, 8});
  const fo::Result<fo::protocol::Frame> result = fo::protocol::decode_frame(
      image.data(), image.size() - 1, fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::ProtocolViolation);
}

FO_TEST(protocol, trailing_bytes_are_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  image.push_back(0);
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
}

FO_TEST(protocol, bad_magic_is_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  image[0] = 'X';
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::ProtocolViolation);
}

FO_TEST(protocol, wrong_version_is_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  put_u16(image, 4, 99);
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::UnsupportedVersion);
}

FO_TEST(protocol, unknown_message_type_is_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  put_u16(image, 6, 4242);
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::ProtocolViolation);
}

FO_TEST(protocol, oversized_declared_payload_is_rejected_before_allocation) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  put_u32(image, 20, 0xFFFFFFF0u);
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::OversizedPayload);
}

FO_TEST(protocol, corrupt_checksum_is_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Hello, 0, 1, {9, 9, 9, 9});
  image[fo::protocol::kFrameHeaderSize] ^= 0xFFu;
  const fo::Result<fo::protocol::Frame> result =
      fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds());
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::IntegrityFailure);
}

FO_TEST(protocol, non_zero_reserved_bytes_are_rejected) {
  std::vector<std::uint8_t> image =
      frame_bytes(fo::protocol::MessageType::Heartbeat, 0, 1, {});
  image[9] = 1;
  FO_CHECK(!fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds()).ok());
  image[9] = 0;
  image[10] = 7;
  FO_CHECK(!fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds()).ok());
  image[10] = 0;
  image[11] = 3;
  FO_CHECK(!fo::protocol::decode_frame(image.data(), image.size(), fo::default_bounds()).ok());
}

FO_TEST(protocol, outgoing_frame_rejects_unknown_types_and_oversize) {
  fo::protocol::Frame frame;
  frame.type = fo::protocol::MessageType::Invalid;
  FO_CHECK(!fo::protocol::encode_frame(frame, fo::default_bounds()).ok());
  frame.type = fo::protocol::MessageType::Hello;
  fo::Bounds bounds = fo::default_bounds();
  bounds.max_frame_bytes = fo::kMinFrameBytes;
  frame.payload.assign(fo::kMinFrameBytes + 1, 0);
  const fo::Result<std::vector<std::uint8_t>> result = fo::protocol::encode_frame(frame, bounds);
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::OversizedPayload);
}

FO_TEST(protocol, hello_round_trip_and_version_check) {
  fo::protocol::HelloRequest request;
  request.client_version = "1.0.0";
  request.publisher = fo::PublisherId::unchecked("publisher-1");
  fo::Encoder encoder;
  fo::protocol::encode(encoder, request);
  const std::vector<std::uint8_t> payload = encoder.take();
  fo::Decoder decoder(payload.data(), payload.size(), 4096);
  const fo::protocol::HelloRequest decoded = FO_UNWRAP(fo::protocol::decode_hello(decoder));
  FO_CHECK_EQ(decoded.client_version, std::string("1.0.0"));
  FO_CHECK_EQ(decoded.publisher.value(), std::string("publisher-1"));

  fo::protocol::HelloRequest bad;
  bad.protocol = 7;
  bad.publisher = fo::PublisherId::unchecked("publisher-1");
  fo::Encoder bad_encoder;
  fo::protocol::encode(bad_encoder, bad);
  const std::vector<std::uint8_t> bad_payload = bad_encoder.take();
  fo::Decoder bad_decoder(bad_payload.data(), bad_payload.size(), 4096);
  const fo::Result<fo::protocol::HelloRequest> result = fo::protocol::decode_hello(bad_decoder);
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::UnsupportedVersion);
}

FO_TEST(protocol, malformed_identifier_in_hello_is_rejected) {
  fo::Encoder encoder;
  encoder.u16(static_cast<std::uint16_t>(fo::protocol::kProtocolVersion));
  encoder.str("1.0.0");
  encoder.str("bad publisher id");
  const std::vector<std::uint8_t> payload = encoder.take();
  fo::Decoder decoder(payload.data(), payload.size(), 4096);
  FO_CHECK(!fo::protocol::decode_hello(decoder).ok());
}

FO_TEST(protocol, publication_ack_round_trip_and_range_checks) {
  fo::protocol::PublicationAck ack;
  ack.disposition = fo::IngestDisposition::Superseded;
  ack.code = fo::ErrorCode::StaleGeneration;
  ack.detail = "older generation";
  ack.watermark = fo::Sequence{17};
  ack.sequence_gap = 3;
  fo::Encoder encoder;
  fo::protocol::encode(encoder, ack);
  const std::vector<std::uint8_t> payload = encoder.take();
  fo::Decoder decoder(payload.data(), payload.size(), 4096);
  const fo::protocol::PublicationAck decoded =
      FO_UNWRAP(fo::protocol::decode_publication_ack(decoder));
  FO_CHECK(decoded.disposition == fo::IngestDisposition::Superseded);
  FO_CHECK(decoded.code == fo::ErrorCode::StaleGeneration);
  FO_CHECK_EQ(decoded.watermark.value(), 17u);
  FO_CHECK_EQ(decoded.sequence_gap, 3u);

  fo::Encoder bad;
  bad.u8(99);
  bad.u8(0);
  bad.str("");
  bad.u64(0);
  bad.u64(0);
  std::vector<std::uint8_t> bad_payload = bad.take();
  fo::Decoder bad_decoder(bad_payload.data(), bad_payload.size(), 4096);
  FO_CHECK(!fo::protocol::decode_publication_ack(bad_decoder).ok());
}

FO_TEST(protocol, reply_payload_round_trip) {
  fo::protocol::ReplyPayload payload;
  payload.code = fo::ErrorCode::Ok;
  payload.message = "ok";
  payload.digest = "abcdef";
  payload.text = "line";
  fo::protocol::WireRow row;
  fo::protocol::WireField field;
  field.name = "key";
  field.value = "value";
  row.fields.push_back(field);
  payload.rows.push_back(row);
  fo::Encoder encoder;
  fo::protocol::encode(encoder, payload);
  const std::vector<std::uint8_t> bytes = encoder.take();
  fo::Decoder decoder(bytes.data(), bytes.size(), 4096);
  const fo::protocol::ReplyPayload decoded = FO_UNWRAP(fo::protocol::decode_reply(decoder));
  FO_CHECK_EQ(decoded.message, std::string("ok"));
  FO_CHECK_EQ(decoded.rows.size(), 1u);
  FO_CHECK_EQ(decoded.rows.front().fields.front().value, std::string("value"));
}

FO_TEST(protocol, stranded_query_round_trip_and_validation) {
  fo::protocol::StrandedQuery query;
  query.request.federation = fo::FederationId::unchecked("fed-1");
  query.request.workload_class = fo::WorkloadClassId::unchecked("wc-1");
  query.request.kind = fo::ResourceKind::Accelerator;
  query.request.include_stale = true;
  fo::Encoder encoder;
  fo::protocol::encode(encoder, query);
  const std::vector<std::uint8_t> bytes = encoder.take();
  fo::Decoder decoder(bytes.data(), bytes.size(), 4096);
  const fo::protocol::StrandedQuery decoded = FO_UNWRAP(fo::protocol::decode_stranded_query(decoder));
  FO_CHECK_EQ(decoded.request.workload_class.value(), std::string("wc-1"));
  FO_CHECK(decoded.request.include_stale);

  fo::Encoder bad;
  bad.str("fed-1");
  bad.str("");
  bad.u8(99);
  std::vector<std::uint8_t> bad_bytes = bad.take();
  fo::Decoder bad_decoder(bad_bytes.data(), bad_bytes.size(), 4096);
  FO_CHECK(!fo::protocol::decode_stranded_query(bad_decoder).ok());
}

FO_TEST(protocol, publication_payload_carries_the_context) {
  const std::vector<std::uint8_t> payload = frame_bytes(
      fo::protocol::MessageType::RegisterFederation, 0, 1, {});
  FO_CHECK(!payload.empty());
  fo::FederationRecord record;
  record.id = fo::FederationId::unchecked("fed-1");
  record.generation = fo::FederationGeneration{1};
  record.coordinator_epoch = fo::CoordinatorEpoch{1};
  fo::PublicationContext context;
  context.publisher = fo::PublisherId::unchecked("publisher-1");
  context.boot = fo::BootGeneration{1};
  context.coordinator_epoch = fo::CoordinatorEpoch{1};
  context.federation = fo::FederationId::unchecked("fed-1");
  context.sequence = fo::Sequence{1};
  fo::Encoder encoder;
  fo::protocol::encode_publication(encoder, context, record);
  const std::vector<std::uint8_t> bytes = encoder.take();
  fo::Decoder decoder(bytes.data(), bytes.size(), 64 * 1024);
  fo::PublicationContext decoded_context;
  const fo::FederationRecord decoded =
      FO_UNWRAP(fo::protocol::decode_federation_publication(decoder, decoded_context));
  FO_CHECK_EQ(decoded.id.value(), std::string("fed-1"));
  FO_CHECK_EQ(decoded_context.publisher.value(), std::string("publisher-1"));
  FO_CHECK_EQ(decoded_context.sequence.value(), 1u);

  fo::Encoder tampered;
  fo::protocol::encode_publication(tampered, context, record);
  std::vector<std::uint8_t> tampered_bytes = tampered.take();
  tampered_bytes.resize(tampered_bytes.size() - 1);
  fo::Decoder tampered_decoder(tampered_bytes.data(), tampered_bytes.size(), 64 * 1024);
  fo::PublicationContext ignored;
  FO_CHECK(!fo::protocol::decode_federation_publication(tampered_decoder, ignored).ok());
}

FO_TEST(protocol, message_type_classification) {
  FO_CHECK(fo::protocol::is_publication(fo::protocol::MessageType::PublishPlacement));
  FO_CHECK(fo::protocol::is_registration(fo::protocol::MessageType::RegisterCluster));
  FO_CHECK(fo::protocol::is_query(fo::protocol::MessageType::QuerySnapshot));
  FO_CHECK(!fo::protocol::is_query(fo::protocol::MessageType::Hello));
  FO_CHECK(!fo::protocol::is_publication(fo::protocol::MessageType::FencePublisher));
  FO_CHECK_EQ(std::string(fo::protocol::to_string(fo::protocol::MessageType::FencePublisher)),
              std::string("FENCE_PUBLISHER"));
}
