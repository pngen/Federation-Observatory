// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "federation_observatory/client.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/publisher.hpp"

namespace fo {

/// A publication target. The synthetic backend is written once against this interface
/// and driven either in-process (examples, unit tests) or over framed TCP against a
/// real coordinator (multiprocess proofs). Synthetic data therefore travels through the
/// identical production analysis pipeline; there is no test-only shortcut path.
class FO_API PublicationSink {
 public:
  PublicationSink() = default;
  virtual ~PublicationSink();

  PublicationSink(const PublicationSink&) = delete;
  PublicationSink& operator=(const PublicationSink&) = delete;

  [[nodiscard]] virtual Status register_self() = 0;
  [[nodiscard]] virtual PublicationContext context(EvidenceGeneration generation) = 0;
  [[nodiscard]] virtual Status emit_federation(FederationRecord record) = 0;
  [[nodiscard]] virtual Status emit_site(SiteRecord record) = 0;
  [[nodiscard]] virtual Status emit_cluster(ClusterRecord record) = 0;
  [[nodiscard]] virtual Status emit_accelerator_class(AcceleratorClassRecord record) = 0;
  [[nodiscard]] virtual Status emit_runtime(RuntimeRecord record) = 0;
  [[nodiscard]] virtual Status emit_backend(BackendRecord record) = 0;
  [[nodiscard]] virtual Status emit_domain(DomainRecord record) = 0;
  [[nodiscard]] virtual Status emit_policy(PolicyRecord record) = 0;
  [[nodiscard]] virtual Status emit_artifact(ArtifactRecord record) = 0;
  [[nodiscard]] virtual Status emit_workload_class(WorkloadClassRecord record) = 0;
  [[nodiscard]] virtual Status emit_workload(WorkloadRecord record) = 0;
  [[nodiscard]] virtual Status emit_capability(const ClusterId& cluster,
                                               ClusterGeneration cluster_generation,
                                               AcceleratorCapabilityGeneration capability_generation,
                                               CapabilitySet capabilities) = 0;
  [[nodiscard]] virtual Status emit_capacity(const ClusterId& cluster,
                                             ClusterGeneration cluster_generation,
                                             CapacityGeneration capacity_generation,
                                             std::vector<CapacityPool> pools) = 0;
  [[nodiscard]] virtual Status emit_placement(PlacementRecord record) = 0;
  [[nodiscard]] virtual Status emit_migration(MigrationRecord record) = 0;
  [[nodiscard]] virtual Status emit_migration_stage(const MigrationId& migration,
                                                    MigrationGeneration generation,
                                                    MigrationStage stage, std::string detail) = 0;
  [[nodiscard]] virtual Status emit_portability(PortabilityAssessment record) = 0;
  [[nodiscard]] virtual Status emit_retire_cluster(const ClusterId& cluster,
                                                   ClusterGeneration generation,
                                                   std::string reason) = 0;
  /// Evidence class this sink's publications carry.
  [[nodiscard]] virtual EvidenceClass evidence_class() const noexcept = 0;
};

/// Sink that publishes into an in-process observatory.
class FO_API ObservatorySink : public PublicationSink {
 public:
  ObservatorySink(FederationObservatory& observatory, PublisherId publisher, BootGeneration boot,
                  FederationId federation);
  ~ObservatorySink() override;

  [[nodiscard]] Status register_self() override;
  [[nodiscard]] PublicationContext context(EvidenceGeneration generation) override;
  [[nodiscard]] Status emit_federation(FederationRecord record) override;
  [[nodiscard]] Status emit_site(SiteRecord record) override;
  [[nodiscard]] Status emit_cluster(ClusterRecord record) override;
  [[nodiscard]] Status emit_accelerator_class(AcceleratorClassRecord record) override;
  [[nodiscard]] Status emit_runtime(RuntimeRecord record) override;
  [[nodiscard]] Status emit_backend(BackendRecord record) override;
  [[nodiscard]] Status emit_domain(DomainRecord record) override;
  [[nodiscard]] Status emit_policy(PolicyRecord record) override;
  [[nodiscard]] Status emit_artifact(ArtifactRecord record) override;
  [[nodiscard]] Status emit_workload_class(WorkloadClassRecord record) override;
  [[nodiscard]] Status emit_workload(WorkloadRecord record) override;
  [[nodiscard]] Status emit_capability(const ClusterId& cluster, ClusterGeneration cluster_generation,
                                       AcceleratorCapabilityGeneration capability_generation,
                                       CapabilitySet capabilities) override;
  [[nodiscard]] Status emit_capacity(const ClusterId& cluster, ClusterGeneration cluster_generation,
                                     CapacityGeneration capacity_generation,
                                     std::vector<CapacityPool> pools) override;
  [[nodiscard]] Status emit_placement(PlacementRecord record) override;
  [[nodiscard]] Status emit_migration(MigrationRecord record) override;
  [[nodiscard]] Status emit_migration_stage(const MigrationId& migration,
                                            MigrationGeneration generation, MigrationStage stage,
                                            std::string detail) override;
  [[nodiscard]] Status emit_portability(PortabilityAssessment record) override;
  [[nodiscard]] Status emit_retire_cluster(const ClusterId& cluster, ClusterGeneration generation,
                                           std::string reason) override;
  [[nodiscard]] EvidenceClass evidence_class() const noexcept override {
    return EvidenceClass::Synthetic;
  }

 private:
  struct State;
  std::unique_ptr<State> state_;
};

/// Sink that publishes over framed TCP through a real publisher process connection.
class FO_API NetworkSink : public PublicationSink {
 public:
  explicit NetworkSink(ObservationPublisher& publisher);
  ~NetworkSink() override;

  [[nodiscard]] Status register_self() override;
  [[nodiscard]] PublicationContext context(EvidenceGeneration generation) override;
  [[nodiscard]] Status emit_federation(FederationRecord record) override;
  [[nodiscard]] Status emit_site(SiteRecord record) override;
  [[nodiscard]] Status emit_cluster(ClusterRecord record) override;
  [[nodiscard]] Status emit_accelerator_class(AcceleratorClassRecord record) override;
  [[nodiscard]] Status emit_runtime(RuntimeRecord record) override;
  [[nodiscard]] Status emit_backend(BackendRecord record) override;
  [[nodiscard]] Status emit_domain(DomainRecord record) override;
  [[nodiscard]] Status emit_policy(PolicyRecord record) override;
  [[nodiscard]] Status emit_artifact(ArtifactRecord record) override;
  [[nodiscard]] Status emit_workload_class(WorkloadClassRecord record) override;
  [[nodiscard]] Status emit_workload(WorkloadRecord record) override;
  [[nodiscard]] Status emit_capability(const ClusterId& cluster, ClusterGeneration cluster_generation,
                                       AcceleratorCapabilityGeneration capability_generation,
                                       CapabilitySet capabilities) override;
  [[nodiscard]] Status emit_capacity(const ClusterId& cluster, ClusterGeneration cluster_generation,
                                     CapacityGeneration capacity_generation,
                                     std::vector<CapacityPool> pools) override;
  [[nodiscard]] Status emit_placement(PlacementRecord record) override;
  [[nodiscard]] Status emit_migration(MigrationRecord record) override;
  [[nodiscard]] Status emit_migration_stage(const MigrationId& migration,
                                            MigrationGeneration generation, MigrationStage stage,
                                            std::string detail) override;
  [[nodiscard]] Status emit_portability(PortabilityAssessment record) override;
  [[nodiscard]] Status emit_retire_cluster(const ClusterId& cluster, ClusterGeneration generation,
                                           std::string reason) override;
  [[nodiscard]] EvidenceClass evidence_class() const noexcept override {
    return EvidenceClass::Synthetic;
  }

 private:
  ObservationPublisher* publisher_ = nullptr;
  bool registered_ = false;
};

/// Deterministic scenario parameters.
struct FO_API SyntheticConfig {
  std::uint64_t seed = 20260101;
  FederationId federation{"synth-fed"};
  std::string display_name = "synthetic-federation";
  PublisherId publisher{"synth-publisher"};
  BootGeneration boot{1};
  /// Prefix applied to every identifier the scenario generates. Two publishers can then
  /// own disjoint cluster sets under one federation, which is what a multi-publisher
  /// federation actually looks like.
  std::string id_prefix;
  std::size_t sites = 3;
  std::size_t clusters_per_site = 2;
  std::size_t accelerator_families = 3;
  bool mixed_runtime_generations = true;
  bool mixed_artifact_compatibility = true;
  bool policy_constraints = true;
  bool topology_constraints = true;

  [[nodiscard]] Status validate() const;
};

/// Ordered scenario steps. Each is independently invocable so that a test can advance
/// the scenario to an exact point.
enum class SyntheticStep : std::uint8_t {
  Topology = 0,
  AcceleratorClasses,
  Runtimes,
  Backends,
  Artifacts,
  WorkloadClasses,
  Policies,
  Domains,
  Workloads,
  Capabilities,
  Capacity,
  Placements,
  Migrations,
  Portability,
  CapabilityChange,
  CapacityChange,
  ClusterJoin,
  ClusterLeave,
  StaleEvidence,
  RepairTopology,
};

inline constexpr std::size_t kSyntheticStepCount = 20;

[[nodiscard]] FO_API std::string_view to_string(SyntheticStep step) noexcept;

/// The deterministic synthetic federation backend. Every identifier it produces is
/// derived from the configured seed and the step index, so two runs with the same
/// configuration publish byte-identical records.
class FO_API SyntheticFederation {
 public:
  SyntheticFederation(SyntheticConfig config, PublicationSink& sink);
  ~SyntheticFederation();
  SyntheticFederation(const SyntheticFederation&) = delete;
  SyntheticFederation& operator=(const SyntheticFederation&) = delete;

  [[nodiscard]] Status run(SyntheticStep step);
  [[nodiscard]] Status run_all();

  [[nodiscard]] const SyntheticConfig& config() const noexcept;
  [[nodiscard]] const FederationId& federation_id() const noexcept;
  [[nodiscard]] const std::vector<SiteId>& site_ids() const noexcept;
  [[nodiscard]] const std::vector<ClusterId>& cluster_ids() const noexcept;
  [[nodiscard]] const std::vector<AcceleratorClassId>& accelerator_class_ids() const noexcept;
  [[nodiscard]] const std::vector<RuntimeId>& runtime_ids() const noexcept;
  [[nodiscard]] const std::vector<ArtifactId>& artifact_ids() const noexcept;
  [[nodiscard]] const std::vector<WorkloadId>& workload_ids() const noexcept;
  [[nodiscard]] const std::vector<WorkloadClassId>& workload_class_ids() const noexcept;
  [[nodiscard]] const std::vector<PlacementId>& placement_ids() const noexcept;
  [[nodiscard]] const std::vector<MigrationId>& migration_ids() const noexcept;
  [[nodiscard]] const std::vector<PolicyId>& policy_ids() const noexcept;

  /// Deterministic scenario expectations that the tests assert against, computed by the
  /// same generator that produced the publications.
  struct Expectations {
    std::uint64_t total_nominal_accelerators = 0;
    std::uint64_t expected_usable_accelerators = 0;
    std::uint64_t expected_stranded_accelerators = 0;
    std::uint64_t expected_unknown_accelerators = 0;
    std::uint64_t fragmentation_required_group = 0;
    bool fragmentation_expected = false;
    std::size_t placements_expected = 0;
    std::size_t rejections_expected = 0;
    std::size_t migrations_expected = 0;
    std::size_t portability_failures_expected = 0;
  };
  [[nodiscard]] const Expectations& expectations() const noexcept;
  [[nodiscard]] const std::string& last_error() const noexcept;

  /// Reproduction record: seed and configuration in a form that can be pasted into a
  /// bug report or replayed by the property tests.
  [[nodiscard]] std::string reproduction_record() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fo
