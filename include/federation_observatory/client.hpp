// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/net.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/protocol.hpp"

namespace fo {

struct FO_API ClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string client_version = FO_VERSION_STRING;
  Bounds bounds;
  int poll_millis = 50;
};

/// A client of a remote coordinator. Every method performs exactly one request/response
/// exchange; there is no client-side cache, so an inspection never reports state the
/// coordinator did not just confirm.
class FO_API FederationClient {
 public:
  FederationClient();
  ~FederationClient();
  FederationClient(FederationClient&&) noexcept;
  FederationClient& operator=(FederationClient&&) noexcept;
  FederationClient(const FederationClient&) = delete;
  FederationClient& operator=(const FederationClient&) = delete;

  [[nodiscard]] static Result<FederationClient> connect(const ClientConfig& config);

  [[nodiscard]] Result<protocol::HelloReply> hello(const PublisherId& publisher);
  [[nodiscard]] Result<protocol::PublicationAck> register_publisher(const PublicationContext& ctx,
                                                                    std::string role);
  [[nodiscard]] Result<protocol::PublicationAck> register_federation(const PublicationContext& ctx,
                                                                     FederationRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_site(const PublicationContext& ctx,
                                                               SiteRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_cluster(const PublicationContext& ctx,
                                                                  ClusterRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_accelerator_class(
      const PublicationContext& ctx, AcceleratorClassRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_runtime(const PublicationContext& ctx,
                                                                  RuntimeRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_backend(const PublicationContext& ctx,
                                                                  BackendRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_domain(const PublicationContext& ctx,
                                                                 DomainRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_policy(const PublicationContext& ctx,
                                                                 PolicyRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_artifact(const PublicationContext& ctx,
                                                                   ArtifactRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_workload_class(
      const PublicationContext& ctx, WorkloadClassRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> register_workload(const PublicationContext& ctx,
                                                                   WorkloadRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> publish_capability(const PublicationContext& ctx,
                                                                    const ClusterId& cluster,
                                                                    ClusterGeneration cluster_generation,
                                                                    AcceleratorCapabilityGeneration capability_generation,
                                                                    CapabilitySet capabilities);
  [[nodiscard]] Result<protocol::PublicationAck> publish_capacity(const PublicationContext& ctx,
                                                                  const ClusterId& cluster,
                                                                  ClusterGeneration cluster_generation,
                                                                  CapacityGeneration capacity_generation,
                                                                  std::vector<CapacityPool> pools);
  [[nodiscard]] Result<protocol::PublicationAck> publish_placement(const PublicationContext& ctx,
                                                                   PlacementRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> publish_migration(const PublicationContext& ctx,
                                                                   MigrationRecord record);
  [[nodiscard]] Result<protocol::PublicationAck> publish_migration_stage(
      const PublicationContext& ctx, const MigrationId& migration, MigrationGeneration generation,
      MigrationStage stage, std::string detail);
  [[nodiscard]] Result<protocol::PublicationAck> publish_portability(const PublicationContext& ctx,
                                                                     PortabilityAssessment record);
  [[nodiscard]] Result<protocol::PublicationAck> retire_cluster(const PublicationContext& ctx,
                                                                const ClusterId& cluster,
                                                                ClusterGeneration generation,
                                                                std::string reason);
  [[nodiscard]] Result<protocol::FenceReply> fence_publisher(const PublisherId& publisher,
                                                             BootGeneration boot, std::string reason,
                                                             std::string token);

  [[nodiscard]] Result<protocol::ReplyPayload> query_snapshot(const protocol::SnapshotQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_placement(const protocol::PlacementQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_rejection(const protocol::RejectionQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_stranded_capacity(
      const protocol::StrandedQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_fragmentation(
      const protocol::FragmentationQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_compatibility(
      const protocol::CompatibilityQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_portability(
      const protocol::PortabilityQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_migration(const protocol::MigrationQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_mismatch(const protocol::MismatchQuery& query);
  [[nodiscard]] Result<protocol::ReplyPayload> query_drift(const protocol::DriftQuery& query);
  /// Ask the coordinator to stop. The coordinator acknowledges and then stops; the
  /// hosting process is what actually exits, so a caller waits on the process.
  [[nodiscard]] Result<protocol::ReplyPayload> request_shutdown();
  [[nodiscard]] Result<protocol::ReplyPayload> query_health();
  [[nodiscard]] Result<protocol::ReplyPayload> query_bounds();

  [[nodiscard]] Status close();
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] const std::string& peer() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fo
