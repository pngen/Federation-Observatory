// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/net.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"

namespace fo::protocol {

/// Wire protocol revision. A peer that presents any other revision is refused during
/// HELLO, before any publication is parsed.
inline constexpr std::uint32_t kProtocolVersion = 1;
/// Fixed frame header size in bytes: magic(4) version(2) type(2) flags(1) reserved(3)
/// request_id(8) payload_length(4) payload_crc(4).
inline constexpr std::size_t kFrameHeaderSize = 28;
inline constexpr char kProtocolMagic[4] = {'F', 'O', 'B', 'S'};
/// Largest accepted length of a client/server version banner.
inline constexpr std::size_t kMaxBannerLength = 128;

enum class FrameFlags : std::uint8_t {
  None = 0,
  Request = 1u << 0,
  Reply = 1u << 1,
  Urgent = 1u << 2,
};

[[nodiscard]] FO_API constexpr std::uint8_t frame_flag(FrameFlags flag) noexcept {
  return static_cast<std::uint8_t>(flag);
}

enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  Heartbeat = 3,
  HeartbeatAck = 4,

  RegisterPublisher = 10,
  RegisterFederation = 11,
  RegisterSite = 12,
  RegisterCluster = 13,
  RegisterAcceleratorClass = 14,
  RegisterRuntime = 15,
  RegisterBackend = 16,
  RegisterDomain = 17,
  RegisterPolicy = 18,
  RegisterArtifact = 19,
  RegisterWorkloadClass = 20,
  RegisterWorkload = 21,

  PublishCapability = 30,
  PublishCapacity = 31,
  PublishPlacement = 32,
  PublishMigration = 33,
  PublishMigrationStage = 34,
  PublishPortability = 35,
  RetireCluster = 36,
  FencePublisher = 37,

  QuerySnapshot = 50,
  QueryPlacement = 51,
  QueryRejection = 52,
  QueryStrandedCapacity = 53,
  QueryFragmentation = 54,
  QueryCompatibility = 55,
  QueryPortability = 56,
  QueryMigration = 57,
  QueryMismatch = 58,
  QueryDrift = 59,
  QueryHealth = 60,
  QueryBounds = 61,

  Reply = 100,
  Failure = 101,
  Shutdown = 102,
};

[[nodiscard]] FO_API std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] FO_API bool is_publication(MessageType type) noexcept;
[[nodiscard]] FO_API bool is_query(MessageType type) noexcept;
[[nodiscard]] FO_API bool is_registration(MessageType type) noexcept;

/// One framed protocol message.
struct FO_API Frame {
  MessageType type = MessageType::Invalid;
  std::uint8_t flags = 0;
  std::uint64_t request_id = 0;
  std::vector<std::uint8_t> payload;

  [[nodiscard]] bool is_request() const noexcept { return (flags & frame_flag(FrameFlags::Request)) != 0; }
  [[nodiscard]] bool is_reply() const noexcept { return (flags & frame_flag(FrameFlags::Reply)) != 0; }
};

/// Encode a single frame, including its header and payload checksum. Rejects unknown
/// message types and payloads larger than \p bounds.max_frame_bytes.
[[nodiscard]] FO_API Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame,
                                                                    const Bounds& bounds);

/// Decode exactly one frame from a complete buffer. The buffer must contain exactly one
/// frame; trailing bytes are a violation. Every rejection happens before any payload
/// field is interpreted.
[[nodiscard]] FO_API Result<Frame> decode_frame(const std::uint8_t* data, std::size_t length,
                                                const Bounds& bounds);

/// Incremental frame reader over a socket. Buffers a partial header or payload across
/// polls; a truncated stream at shutdown is reported, never silently ignored.
class FO_API FrameReader {
 public:
  FrameReader(net::Socket& socket, const Bounds& bounds) : socket_(&socket), bounds_(&bounds) {}

  /// Read one frame, polling the socket every \p poll_millis while no data arrives.
  [[nodiscard]] Result<Frame> next(int poll_millis);

  /// Bytes buffered from the current partial frame, for diagnostics.
  [[nodiscard]] std::size_t buffered() const noexcept { return buffered_; }

 private:
  net::Socket* socket_ = nullptr;
  const Bounds* bounds_ = nullptr;
  std::vector<std::uint8_t> buffer_;
  std::size_t buffered_ = 0;
  bool header_consumed_ = false;
};

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

struct FO_API HelloRequest {
  std::uint16_t protocol = kProtocolVersion;
  std::string client_version;
  PublisherId publisher;
};

struct FO_API HelloReply {
  std::uint16_t protocol = kProtocolVersion;
  std::string server_version;
  CoordinatorEpoch epoch;
  std::string coordinator_id;
};

struct FO_API RegisterPublisherRequest {
  PublicationContext context;
  std::string role;
};

struct FO_API PublicationAck {
  IngestDisposition disposition = IngestDisposition::Applied;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  Sequence watermark;
  std::uint64_t sequence_gap = 0;
};

struct FO_API WireField {
  std::string name;
  std::string value;
};

struct FO_API WireRow {
  std::vector<WireField> fields;
};

/// Result of any query. Structured rows plus a deterministic text rendering and a
/// content digest. The in-process C++ API returns fully typed analysis objects; the
/// wire surface returns the same conclusions in a bounded, versioned tabular form.
struct FO_API ReplyPayload {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  std::string detail;
  std::string digest;
  std::string text;
  std::vector<WireRow> rows;
};

struct FO_API FenceRequest {
  PublisherId publisher;
  BootGeneration boot;
  std::string reason;
  std::string token;
};

struct FO_API RetireClusterRequest {
  PublicationContext context;
  ClusterId cluster;
  ClusterGeneration generation;
  std::string reason;
};

struct FO_API MigrationStageRequest {
  PublicationContext context;
  MigrationId migration;
  MigrationGeneration generation;
  MigrationStage stage = MigrationStage::Planned;
  std::string detail;
};

struct FO_API CapabilityPublicationRequest {
  PublicationContext context;
  ClusterId cluster;
  ClusterGeneration cluster_generation;
  AcceleratorCapabilityGeneration capability_generation;
  CapabilitySet capabilities;
};

struct FO_API CapacityPublicationRequest {
  PublicationContext context;
  ClusterId cluster;
  ClusterGeneration cluster_generation;
  CapacityGeneration capacity_generation;
  std::vector<CapacityPool> pools;
};

struct FO_API SnapshotQuery {
  FederationId federation;
  std::uint64_t max_records = 4096;
  bool include_history = false;
};

struct FO_API PlacementQuery {
  PlacementId placement;
  WorkloadId workload;
  bool latest_by_workload = false;
};

struct FO_API RejectionQuery {
  PlacementId placement;
  ClusterId candidate;
};

struct FO_API StrandedQuery {
  StrandedCapacityRequest request;
};

struct FO_API FragmentationQuery {
  FederationId federation;
  WorkloadClassId workload_class;
  ResourceKind kind = ResourceKind::Accelerator;
};

struct FO_API CompatibilityQuery {
  ClusterId cluster;
  WorkloadId workload;
};

struct FO_API PortabilityQuery {
  WorkloadId workload;
  ClusterId destination;
  /// When true the runtime computes a fresh assessment instead of returning the last
  /// published one.
  bool evaluate = false;
};

struct FO_API MigrationQuery {
  MigrationId migration;
};

struct FO_API MismatchQuery {
  MismatchAnalysisRequest request;
};

struct FO_API DriftQuery {
  FederationId federation;
  bool have_intended = false;
  IntendedFederationState intended;
  bool behavior_window_supplied = false;
  TimeWindow before_window;
  TimeWindow after_window;
};

struct FO_API FenceReply {
  bool fenced = false;
  std::string detail;
};

// Encoding and decoding. Every decoder validates bounds and semantics; a decoded
// message is either fully valid or an Error.
FO_API void encode(Encoder& out, const HelloRequest& value);
FO_API Result<HelloRequest> decode_hello(Decoder& in);
FO_API void encode(Encoder& out, const HelloReply& value);
FO_API Result<HelloReply> decode_hello_reply(Decoder& in);
FO_API void encode(Encoder& out, const RegisterPublisherRequest& value);
FO_API Result<RegisterPublisherRequest> decode_register_publisher(Decoder& in);
FO_API void encode(Encoder& out, const PublicationAck& value);
FO_API Result<PublicationAck> decode_publication_ack(Decoder& in);
FO_API void encode(Encoder& out, const ReplyPayload& value);
FO_API Result<ReplyPayload> decode_reply(Decoder& in);
FO_API void encode(Encoder& out, const FenceRequest& value);
FO_API Result<FenceRequest> decode_fence(Decoder& in);
FO_API void encode(Encoder& out, const RetireClusterRequest& value);
FO_API Result<RetireClusterRequest> decode_retire_cluster(Decoder& in);
FO_API void encode(Encoder& out, const MigrationStageRequest& value);
FO_API Result<MigrationStageRequest> decode_migration_stage(Decoder& in);
FO_API void encode(Encoder& out, const CapabilityPublicationRequest& value);
FO_API Result<CapabilityPublicationRequest> decode_capability_publication(Decoder& in);
FO_API void encode(Encoder& out, const CapacityPublicationRequest& value);
FO_API Result<CapacityPublicationRequest> decode_capacity_publication(Decoder& in);
FO_API void encode(Encoder& out, const SnapshotQuery& value);
FO_API Result<SnapshotQuery> decode_snapshot_query(Decoder& in);
FO_API void encode(Encoder& out, const PlacementQuery& value);
FO_API Result<PlacementQuery> decode_placement_query(Decoder& in);
FO_API void encode(Encoder& out, const RejectionQuery& value);
FO_API Result<RejectionQuery> decode_rejection_query(Decoder& in);
FO_API void encode(Encoder& out, const StrandedQuery& value);
FO_API Result<StrandedQuery> decode_stranded_query(Decoder& in);
FO_API void encode(Encoder& out, const FragmentationQuery& value);
FO_API Result<FragmentationQuery> decode_fragmentation_query(Decoder& in);
FO_API void encode(Encoder& out, const CompatibilityQuery& value);
FO_API Result<CompatibilityQuery> decode_compatibility_query(Decoder& in);
FO_API void encode(Encoder& out, const PortabilityQuery& value);
FO_API Result<PortabilityQuery> decode_portability_query(Decoder& in);
FO_API void encode(Encoder& out, const MigrationQuery& value);
FO_API Result<MigrationQuery> decode_migration_query(Decoder& in);
FO_API void encode(Encoder& out, const MismatchQuery& value);
FO_API Result<MismatchQuery> decode_mismatch_query(Decoder& in);
FO_API void encode(Encoder& out, const DriftQuery& value);
FO_API Result<DriftQuery> decode_drift_query(Decoder& in);

// Publication record payloads.
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const FederationRecord& r);
FO_API Result<FederationRecord> decode_federation_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const SiteRecord& r);
FO_API Result<SiteRecord> decode_site_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const ClusterRecord& r);
FO_API Result<ClusterRecord> decode_cluster_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx,
                               const AcceleratorClassRecord& r);
FO_API Result<AcceleratorClassRecord> decode_accelerator_class_publication(Decoder& in,
                                                                          PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const RuntimeRecord& r);
FO_API Result<RuntimeRecord> decode_runtime_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const BackendRecord& r);
FO_API Result<BackendRecord> decode_backend_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const DomainRecord& r);
FO_API Result<DomainRecord> decode_domain_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const PolicyRecord& r);
FO_API Result<PolicyRecord> decode_policy_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const ArtifactRecord& r);
FO_API Result<ArtifactRecord> decode_artifact_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx,
                               const WorkloadClassRecord& r);
FO_API Result<WorkloadClassRecord> decode_workload_class_publication(Decoder& in,
                                                                    PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const WorkloadRecord& r);
FO_API Result<WorkloadRecord> decode_workload_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const PlacementRecord& r);
FO_API Result<PlacementRecord> decode_placement_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx, const MigrationRecord& r);
FO_API Result<MigrationRecord> decode_migration_publication(Decoder& in, PublicationContext& ctx);
FO_API void encode_publication(Encoder& out, const PublicationContext& ctx,
                               const PortabilityAssessment& r);
FO_API Result<PortabilityAssessment> decode_portability_publication(Decoder& in,
                                                                    PublicationContext& ctx);

}  // namespace fo::protocol
