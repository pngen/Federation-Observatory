// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/capability.hpp"
#include "federation_observatory/capacity.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"

namespace fo {

/// How current a record's dynamic evidence is.
///
/// This is the single most important distinction in the runtime. After a coordinator
/// restart, nothing dynamic is Current: historical records keep their values but their
/// currentness degrades, and only fresh publisher evidence can restore it.
enum class Currentness : std::uint8_t {
  Unknown = 0,              ///< No evidence about freshness.
  Current = 1,              ///< Evidence published by a live publisher of the current epoch.
  Stale = 2,                ///< Evidence exists but is superseded (dead publisher, newer generation, or age).
  RevalidationRequired = 3, ///< Restored from persistence; must be republished before use.
  Retired = 4,              ///< Cluster/site left the federation. Retained as history only.
};

[[nodiscard]] FO_API std::string_view to_string(Currentness currentness) noexcept;
[[nodiscard]] FO_API bool parse_currentness(std::string_view text, Currentness& out) noexcept;
/// True only for Currentness::Current.
[[nodiscard]] FO_API bool is_current(Currentness currentness) noexcept;

/// Cluster health/readiness as consumed from an owning controller.
enum class Readiness : std::uint8_t {
  Unknown = 0,
  Ready = 1,
  NotReady = 2,
  Draining = 3,
  Retired = 4,
};

[[nodiscard]] FO_API std::string_view to_string(Readiness readiness) noexcept;
[[nodiscard]] FO_API bool parse_readiness(std::string_view text, Readiness& out) noexcept;

/// Accelerator runtime family of a runtime record.
enum class RuntimeKind : std::uint8_t {
  Unknown = 0,
  Cuda = 1,
  Rocm = 2,
  OneApi = 3,
  Metal = 4,
  Vulkan = 5,
  OpenCl = 6,
  CpuGeneric = 7,
  Custom = 8,
};

[[nodiscard]] FO_API std::string_view to_string(RuntimeKind kind) noexcept;
[[nodiscard]] FO_API bool parse_runtime_kind(std::string_view text, RuntimeKind& out) noexcept;

/// Kind of artifact whose compatibility is being observed.
enum class ArtifactKind : std::uint8_t {
  Unknown = 0,
  ExecutableBinary = 1,
  SharedLibrary = 2,
  DeviceImage = 3,   ///< Compiled device code (cubin/fatbin/hsaco).
  ContainerImage = 4,
  ModelWeights = 5,
  Adapter = 6,
  Checkpoint = 7,
  DatasetShard = 8,
  Custom = 9,
};

[[nodiscard]] FO_API std::string_view to_string(ArtifactKind kind) noexcept;
[[nodiscard]] FO_API bool parse_artifact_kind(std::string_view text, ArtifactKind& out) noexcept;

/// Scope of a placement domain.
enum class DomainKind : std::uint8_t {
  Unknown = 0,
  Cluster = 1,      ///< One cluster is one placement domain.
  Site = 2,         ///< Co-dependency may span clusters within one site.
  Federation = 3,   ///< Co-dependency may span sites.
};

[[nodiscard]] FO_API std::string_view to_string(DomainKind kind) noexcept;
[[nodiscard]] FO_API bool parse_domain_kind(std::string_view text, DomainKind& out) noexcept;

/// Provenance stamp attached to every published observation.
struct FO_API ObservationStamp {
  PublisherId publisher;
  BootGeneration publisher_boot;
  CoordinatorEpoch coordinator_epoch;
  EvidenceGeneration evidence_generation;
  Sequence sequence;
  TimestampNanos observed_at = 0;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  Provenance provenance = Provenance::Unknown;

  [[nodiscard]] bool valid() const noexcept { return !publisher.empty(); }
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
  friend bool operator==(const ObservationStamp& a, const ObservationStamp& b) noexcept;
};

/// A federation: the top-level observational scope.
struct FO_API FederationRecord {
  FederationId id;
  FederationGeneration generation;
  std::string display_name;
  CoordinatorEpoch coordinator_epoch;
  std::vector<SiteId> sites;
  std::vector<ClusterId> clusters;
  std::vector<AcceleratorClassId> accelerator_classes;
  std::vector<RuntimeId> runtimes;
  std::vector<WorkloadClassId> workload_classes;
  PolicyId policy;
  PolicyGeneration policy_generation;
  CompatibilityGeneration compatibility_generation;
  CapacityGeneration capacity_generation;
  TopologyGeneration topology_generation;
  EvidenceGeneration evidence_generation;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool contains_cluster(const ClusterId& cluster) const;
  [[nodiscard]] bool contains_site(const SiteId& site) const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A site: a physical or administrative grouping of clusters.
struct FO_API SiteRecord {
  SiteId id;
  SiteGeneration generation;
  FederationId federation;
  std::string region;
  std::string zone;
  std::string failure_domain;
  std::vector<ClusterId> clusters;
  std::vector<PolicyId> policies;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// An accelerator class: one family/model of accelerator with its own capability
/// generation. Capacity pools reference these; compatibility analysis is per class.
struct FO_API AcceleratorClassRecord {
  AcceleratorClassId id;
  AcceleratorCapabilityGeneration capability_generation;
  std::string vendor;
  std::string family;
  std::string model;
  /// Vendor architecture token, e.g. "sm_120", "gfx942", "x86_64".
  std::string architecture;
  /// Dotted compute capability, e.g. "12.0". Empty when not applicable.
  std::string compute_capability;
  std::uint64_t memory_bytes_per_device = 0;
  std::uint32_t memory_bandwidth_gbps = 0;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A runtime/backend stack: the software generation that must match an artifact.
struct FO_API RuntimeRecord {
  RuntimeId id;
  RuntimeGeneration generation;
  BackendId backend;
  BackendGeneration backend_generation;
  RuntimeKind kind = RuntimeKind::Unknown;
  std::string version;
  std::string abi;
  std::string driver_version;
  std::string driver_abi;
  std::string compiler_version;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A serving backend identity (model-format consumer).
struct FO_API BackendRecord {
  BackendId id;
  BackendGeneration generation;
  RuntimeId runtime;
  std::string name;
  std::string version;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A cluster: the unit that publishes accelerator, runtime and capacity evidence.
struct FO_API ClusterRecord {
  ClusterId id;
  ClusterGeneration generation;
  ClusterEpoch epoch;
  FederationId federation;
  SiteId site;
  DomainId domain;
  std::string failure_domain;
  std::string zone;

  std::vector<AcceleratorClassId> accelerator_classes;
  std::vector<CapacityPool> capacity_pools;
  std::vector<RuntimeId> runtimes;
  std::vector<BackendId> backends;

  TopologyGeneration topology_generation;
  AcceleratorCapabilityGeneration capability_generation;
  CapacityGeneration capacity_generation;
  RuntimeGeneration runtime_generation;
  CompatibilityGeneration compatibility_generation;
  EvidenceGeneration evidence_generation;

  Readiness readiness = Readiness::Unknown;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] const CapacityPool* find_pool(const ResourcePoolId& pool) const;
  /// Sum all accelerator pools. Uses checked arithmetic and fails on overflow.
  [[nodiscard]] Result<CapacityLedger> total_ledger(ResourceKind kind) const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// An intended- or observed-membership domain used for locality and fragmentation
/// analysis. A domain is the finest region inside which a scheduler may place a
/// co-dependent group of resources.
struct FO_API DomainRecord {
  DomainId id;
  DomainKind kind = DomainKind::Unknown;
  FederationId federation;
  SiteId site;
  std::vector<ClusterId> clusters;
  TopologyGeneration topology_generation;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// An artifact whose compatibility with clusters and runtimes is observed.
struct FO_API ArtifactRecord {
  ArtifactId id;
  ArtifactGeneration generation;
  ArtifactKind kind = ArtifactKind::Unknown;
  std::string format;
  std::string kernel_format;
  std::vector<std::string> target_architectures;
  std::string minimum_compute_capability;
  std::uint64_t size_bytes = 0;
  std::string runtime_abi;
  std::string driver_abi;
  std::string compiler_target;
  std::string state_format;
  std::uint64_t required_memory_bytes = 0;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool targets_architecture(std::string_view architecture) const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A workload class: the granularity at which stranding is computed.
struct FO_API WorkloadClassRecord {
  WorkloadClassId id;
  std::string display_name;
  bool prefers_accelerators = true;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A workload generation: exactly which requirements this generation carries.
struct FO_API WorkloadRecord {
  WorkloadId id;
  WorkloadGeneration generation;
  WorkloadClassId workload_class;
  ArtifactId artifact;
  ArtifactGeneration artifact_generation;
  std::uint32_t required_accelerators = 1;
  std::uint64_t required_memory_bytes_per_accelerator = 0;
  std::vector<AcceleratorClassId> acceptable_accelerator_classes;
  std::vector<CapabilityRequirement> requirements;
  PolicyId policy;
  PolicyGeneration policy_generation;
  /// Collective/topology scope: the domain within which the group must co-reside.
  DomainId required_domain;
  DomainKind required_domain_kind = DomainKind::Unknown;
  bool require_single_failure_domain = false;
  std::string isolation_requirement;
  std::string portability_class;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// A placement / admission policy generation.
struct FO_API PolicyRecord {
  PolicyId id;
  PolicyGeneration generation;
  std::string name;
  std::vector<SiteId> allowed_sites;
  std::vector<SiteId> denied_sites;
  std::vector<ClusterId> allowed_clusters;
  std::vector<ClusterId> denied_clusters;
  /// Whether cross-site co-dependency is permitted. Unknown when unpublished.
  Tri allow_cross_site = Tri::Unknown;
  std::string data_residency;
  std::string isolation_requirement;
  CapabilitySet capabilities;
  Currentness currentness = Currentness::Unknown;
  ObservationStamp stamp;
  EvidenceList evidence;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] Tri permits_site(const SiteId& site) const;
  [[nodiscard]] Tri permits_cluster(const ClusterId& cluster) const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

}  // namespace fo
