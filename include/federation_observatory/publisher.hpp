// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "federation_observatory/client.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/net.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/protocol.hpp"

namespace fo {

struct FO_API PublisherConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  PublisherId publisher{"publisher"};
  /// Fresh per process incarnation. A restarted publisher must present a new value.
  BootGeneration boot{1};
  FederationId federation{"federation"};
  FederationGeneration federation_generation{1};
  std::string role = "cluster-controller";
  EvidenceClass evidence_class = EvidenceClass::Synthetic;
  Provenance provenance = Provenance::SyntheticBackend;
  Precision precision = Precision::Exact;
  Bounds bounds;
  int poll_millis = 50;
};

/// A publishing agent. Owns one connection, one monotonically increasing sequence
/// counter and one boot identity; it never re-uses a sequence number and never
/// re-uses a boot identity.
class FO_API ObservationPublisher {
 public:
  ObservationPublisher();
  ~ObservationPublisher();
  ObservationPublisher(ObservationPublisher&&) noexcept;
  ObservationPublisher& operator=(ObservationPublisher&&) noexcept;
  ObservationPublisher(const ObservationPublisher&) = delete;
  ObservationPublisher& operator=(const ObservationPublisher&) = delete;

  [[nodiscard]] static Result<ObservationPublisher> connect(const PublisherConfig& config);

  [[nodiscard]] const PublisherConfig& config() const noexcept;
  [[nodiscard]] Sequence last_accepted_sequence() const noexcept;
  [[nodiscard]] Sequence next_sequence() noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;

  /// Build a context with the next sequence number. Every publication must take a fresh
  /// context; re-using one is a programming error the coordinator rejects as a
  /// duplicate.
  [[nodiscard]] PublicationContext make_context(EvidenceGeneration evidence_generation);

  [[nodiscard]] Status register_self();
  [[nodiscard]] Status heartbeat();

  [[nodiscard]] Status publish_federation(FederationRecord record);
  [[nodiscard]] Status publish_site(SiteRecord record);
  [[nodiscard]] Status publish_cluster(ClusterRecord record);
  [[nodiscard]] Status publish_accelerator_class(AcceleratorClassRecord record);
  [[nodiscard]] Status publish_runtime(RuntimeRecord record);
  [[nodiscard]] Status publish_backend(BackendRecord record);
  [[nodiscard]] Status publish_domain(DomainRecord record);
  [[nodiscard]] Status publish_policy(PolicyRecord record);
  [[nodiscard]] Status publish_artifact(ArtifactRecord record);
  [[nodiscard]] Status publish_workload_class(WorkloadClassRecord record);
  [[nodiscard]] Status publish_workload(WorkloadRecord record);
  [[nodiscard]] Status publish_capability(const ClusterId& cluster,
                                          ClusterGeneration cluster_generation,
                                          AcceleratorCapabilityGeneration capability_generation,
                                          CapabilitySet capabilities);
  [[nodiscard]] Status publish_capacity(const ClusterId& cluster,
                                        ClusterGeneration cluster_generation,
                                        CapacityGeneration capacity_generation,
                                        std::vector<CapacityPool> pools);
  [[nodiscard]] Status publish_placement(PlacementRecord record);
  [[nodiscard]] Status publish_migration(MigrationRecord record);
  [[nodiscard]] Status publish_migration_stage(const MigrationId& migration,
                                               MigrationGeneration generation, MigrationStage stage,
                                               std::string detail);
  [[nodiscard]] Status publish_portability(PortabilityAssessment record);
  [[nodiscard]] Status retire_cluster(const ClusterId& cluster, ClusterGeneration generation,
                                      std::string reason);

  [[nodiscard]] FederationClient& client() noexcept;
  [[nodiscard]] Status close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Entry point used by the fo-publisher executable.
FO_API int run_publisher_main(int argc, char** argv);

}  // namespace fo
