// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Deterministic synthetic federation. The scenario is written once against
// PublicationSink, so it is driven either in-process or over framed TCP against a real
// coordinator. There is no test-only path: synthetic evidence travels through exactly
// the same validation, generation fencing and analysis as any other publication.

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/portability.hpp"
#include "federation_observatory/synthetic.hpp"

namespace fo {
namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

struct FamilySpec {
  const char* vendor;
  const char* family;
  const char* model;
  const char* architecture;
  const char* compute_capability;
  std::uint64_t memory_bytes;
  std::uint32_t bandwidth_gbps;
  bool fp8;
  bool fp32;
  bool bf16;
  const char* runtime_kind;
  const char* runtime_version;
  const char* runtime_abi;
  const char* driver_version;
  const char* driver_abi;
  const char* compiler_target;
  const char* partition_mode;
  std::uint32_t devices_per_cluster;
};

constexpr FamilySpec kFamilies[] = {
    // Eight devices per cluster in the first family, so that the federation has enough
    // aggregate capacity for an eight-device co-dependent group while no single site
    // domain holds eight of them: that is the fragmentation this scenario demonstrates.
    {"nvidia", "hopper", "h100-sxm", "sm_90", "9.0", 80 * kGiB, 3350, true, true, true,
     "CUDA", "12.4.1", "cuda-12.4", "550.54.15", "550.54", "sm_90", "mig-7g", 8},
    {"amd", "cdna3", "mi300x", "gfx942", "", 192 * kGiB, 5300, false, true, true, "ROCM", "6.2.0",
     "rocm-6.2", "6.2.0", "6.2", "gfx942", "cpX", 4},
    {"nvidia", "ada", "l40s", "sm_89", "8.9", 48 * kGiB, 864, true, true, true, "CUDA", "12.9.0",
     "cuda-12.9", "560.28.03", "560.28", "sm_89", "mig-4g", 4},
};

constexpr std::size_t kFamilyCount = sizeof(kFamilies) / sizeof(kFamilies[0]);
constexpr std::uint32_t kExtraFamilyTwoDevices = 4;

std::string index_suffix(std::size_t value) { return std::to_string(value); }

RuntimeKind runtime_kind_from(const char* text) {
  RuntimeKind kind = RuntimeKind::Unknown;
  const Status parsed = [&] {
    return parse_runtime_kind(text, kind) ? Status::success()
                                          : fail(ErrorCode::InvalidArgument, "bad runtime kind");
  }();
  (void)parsed;
  return kind;
}

CapabilityEntry make_entry(CapabilityKey key, CapabilityValue value, EvidenceClass cls,
                           Precision precision, EvidenceGeneration generation,
                           std::string note = {}) {
  CapabilityEntry entry;
  entry.key.key = key;
  entry.value = std::move(value);
  entry.evidence_class = cls;
  entry.precision = precision;
  entry.generation = generation;
  entry.note = std::move(note);
  return entry;
}

}  // namespace

Status SyntheticConfig::validate() const {
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "synthetic configuration has no federation");
  }
  if (publisher.empty()) {
    return fail(ErrorCode::InvalidArgument, "synthetic configuration has no publisher");
  }
  if (!boot.is_set()) {
    return fail(ErrorCode::InvalidArgument, "synthetic configuration has no boot identity");
  }
  if (sites == 0 || sites > 16) {
    return fail(ErrorCode::InvalidArgument, "synthetic site count must be between 1 and 16");
  }
  if (clusters_per_site == 0 || clusters_per_site > 16) {
    return fail(ErrorCode::InvalidArgument,
                "synthetic clusters per site must be between 1 and 16");
  }
  if (accelerator_families == 0 || accelerator_families > kFamilyCount) {
    return fail(ErrorCode::InvalidArgument, "synthetic accelerator family count must be between 1 and",
                std::to_string(kFamilyCount));
  }
  return Status::success();
}

std::string_view to_string(SyntheticStep step) noexcept {
  switch (step) {
    case SyntheticStep::Topology: return "topology";
    case SyntheticStep::AcceleratorClasses: return "accelerator-classes";
    case SyntheticStep::Runtimes: return "runtimes";
    case SyntheticStep::Backends: return "backends";
    case SyntheticStep::Artifacts: return "artifacts";
    case SyntheticStep::WorkloadClasses: return "workload-classes";
    case SyntheticStep::Policies: return "policies";
    case SyntheticStep::Domains: return "domains";
    case SyntheticStep::Workloads: return "workloads";
    case SyntheticStep::Capabilities: return "capabilities";
    case SyntheticStep::Capacity: return "capacity";
    case SyntheticStep::Placements: return "placements";
    case SyntheticStep::Migrations: return "migrations";
    case SyntheticStep::Portability: return "portability";
    case SyntheticStep::CapabilityChange: return "capability-change";
    case SyntheticStep::CapacityChange: return "capacity-change";
    case SyntheticStep::ClusterJoin: return "cluster-join";
    case SyntheticStep::ClusterLeave: return "cluster-leave";
    case SyntheticStep::StaleEvidence: return "stale-evidence";
    case SyntheticStep::RepairTopology: return "repair-topology";
  }
  return "topology";
}

PublicationSink::~PublicationSink() = default;

// ---------------------------------------------------------------------------
// ObservatorySink
// ---------------------------------------------------------------------------

struct ObservatorySink::State {
  FederationObservatory* observatory = nullptr;
  PublisherId publisher;
  BootGeneration boot;
  FederationId federation;
  Sequence sequence;
  bool registered = false;
};

ObservatorySink::ObservatorySink(FederationObservatory& observatory, PublisherId publisher,
                                 BootGeneration boot, FederationId federation)
    : state_(std::make_unique<State>()) {
  state_->observatory = &observatory;
  state_->publisher = std::move(publisher);
  state_->boot = boot;
  state_->federation = std::move(federation);
}

ObservatorySink::~ObservatorySink() = default;

Status ObservatorySink::register_self() {
  // Idempotent: a sink that already registered this boot identity does not re-register, so
  // a caller may register explicitly and then run a scenario that registers too.
  if (state_->registered) {
    return Status::success();
  }
  PublicationContext context = this->context(EvidenceGeneration{1});
  const Result<IngestResult> result = state_->observatory->register_publisher(context);
  if (!result.ok()) {
    return result.error();
  }
  state_->registered = true;
  return Status::success();
}

PublicationContext ObservatorySink::context(EvidenceGeneration generation) {
  PublicationContext context;
  context.publisher = state_->publisher;
  context.boot = state_->boot;
  context.coordinator_epoch = state_->observatory->coordinator_epoch();
  context.federation = state_->federation;
  context.federation_generation = FederationGeneration{1};
  state_->sequence = state_->sequence.next();
  context.sequence = state_->sequence;
  context.observed_at = now_unix_nanos();
  context.precision = Precision::Exact;
  context.evidence_class = EvidenceClass::Synthetic;
  context.provenance = Provenance::SyntheticBackend;
  context.evidence_generation = generation;
  return context;
}

namespace {

Status absorb(const Result<IngestResult>& result, const char* what) {
  if (!result.ok()) {
    return fail(result.error().code(), std::string("synthetic ") + what + " refused: " +
                                           result.error().message(),
                result.error().detail());
  }
  switch (result.value().disposition) {
    case IngestDisposition::Applied:
    case IngestDisposition::Duplicate:
    case IngestDisposition::Superseded:
    case IngestDisposition::Deferred:
      return Status::success();
  }
  return fail(ErrorCode::Internal, "unknown ingest disposition");
}

}  // namespace

#define FO_OBSERVATORY_SINK_EMIT(MethodName, RecordType, ObservatoryMethod)                    \
  Status ObservatorySink::MethodName(RecordType record) {                                      \
    return absorb(state_->observatory->ObservatoryMethod(context(EvidenceGeneration{1}),        \
                                                         std::move(record)),                   \
                  #MethodName);                                                                \
  }

FO_OBSERVATORY_SINK_EMIT(emit_federation, FederationRecord, register_federation)
FO_OBSERVATORY_SINK_EMIT(emit_site, SiteRecord, register_site)
FO_OBSERVATORY_SINK_EMIT(emit_cluster, ClusterRecord, register_cluster)
FO_OBSERVATORY_SINK_EMIT(emit_accelerator_class, AcceleratorClassRecord, register_accelerator_class)
FO_OBSERVATORY_SINK_EMIT(emit_runtime, RuntimeRecord, register_runtime)
FO_OBSERVATORY_SINK_EMIT(emit_backend, BackendRecord, register_backend)
FO_OBSERVATORY_SINK_EMIT(emit_domain, DomainRecord, register_domain)
FO_OBSERVATORY_SINK_EMIT(emit_policy, PolicyRecord, register_policy)
FO_OBSERVATORY_SINK_EMIT(emit_artifact, ArtifactRecord, register_artifact)
FO_OBSERVATORY_SINK_EMIT(emit_workload_class, WorkloadClassRecord, register_workload_class)
FO_OBSERVATORY_SINK_EMIT(emit_workload, WorkloadRecord, register_workload)
FO_OBSERVATORY_SINK_EMIT(emit_placement, PlacementRecord, publish_placement)
FO_OBSERVATORY_SINK_EMIT(emit_migration, MigrationRecord, publish_migration)
FO_OBSERVATORY_SINK_EMIT(emit_portability, PortabilityAssessment, publish_portability)

#undef FO_OBSERVATORY_SINK_EMIT

Status ObservatorySink::emit_capability(const ClusterId& cluster,
                                        ClusterGeneration cluster_generation,
                                        AcceleratorCapabilityGeneration capability_generation,
                                        CapabilitySet capabilities) {
  return absorb(state_->observatory->publish_capability(context(EvidenceGeneration{1}), cluster,
                                                        cluster_generation, capability_generation,
                                                        std::move(capabilities)),
                "capability");
}

Status ObservatorySink::emit_capacity(const ClusterId& cluster,
                                      ClusterGeneration cluster_generation,
                                      CapacityGeneration capacity_generation,
                                      std::vector<CapacityPool> pools) {
  return absorb(state_->observatory->publish_capacity(context(EvidenceGeneration{1}), cluster,
                                                      cluster_generation, capacity_generation,
                                                      std::move(pools)),
                "capacity");
}

Status ObservatorySink::emit_migration_stage(const MigrationId& migration,
                                             MigrationGeneration generation, MigrationStage stage,
                                             std::string detail) {
  return absorb(state_->observatory->publish_migration_stage(
                    context(EvidenceGeneration{1}), migration, generation, stage, std::move(detail)),
                "migration stage");
}

Status ObservatorySink::emit_retire_cluster(const ClusterId& cluster, ClusterGeneration generation,
                                            std::string reason) {
  return absorb(state_->observatory->retire_cluster(context(EvidenceGeneration{1}), cluster,
                                                    generation, std::move(reason)),
                "cluster retirement");
}

// ---------------------------------------------------------------------------
// NetworkSink
// ---------------------------------------------------------------------------

NetworkSink::NetworkSink(ObservationPublisher& publisher) : publisher_(&publisher) {}
NetworkSink::~NetworkSink() = default;

Status NetworkSink::register_self() {
  if (registered_) {
    return Status::success();
  }
  const Status status = publisher_->register_self();
  if (status.ok()) {
    registered_ = true;
  }
  return status;
}

PublicationContext NetworkSink::context(EvidenceGeneration generation) {
  return publisher_->make_context(generation);
}

namespace {

Status absorb_status(const Status& status, const char* what) {
  if (!status.ok()) {
    return fail(status.error().code(), std::string("synthetic ") + what + " refused: " +
                                           status.error().message(),
                status.error().detail());
  }
  return Status::success();
}

}  // namespace

#define FO_NETWORK_SINK_EMIT(MethodName, RecordType, PublisherMethod)                \
  Status NetworkSink::MethodName(RecordType record) {                                \
    return absorb_status(publisher_->PublisherMethod(std::move(record)), #MethodName); \
  }

FO_NETWORK_SINK_EMIT(emit_federation, FederationRecord, publish_federation)
FO_NETWORK_SINK_EMIT(emit_site, SiteRecord, publish_site)
FO_NETWORK_SINK_EMIT(emit_cluster, ClusterRecord, publish_cluster)
FO_NETWORK_SINK_EMIT(emit_accelerator_class, AcceleratorClassRecord, publish_accelerator_class)
FO_NETWORK_SINK_EMIT(emit_runtime, RuntimeRecord, publish_runtime)
FO_NETWORK_SINK_EMIT(emit_backend, BackendRecord, publish_backend)
FO_NETWORK_SINK_EMIT(emit_domain, DomainRecord, publish_domain)
FO_NETWORK_SINK_EMIT(emit_policy, PolicyRecord, publish_policy)
FO_NETWORK_SINK_EMIT(emit_artifact, ArtifactRecord, publish_artifact)
FO_NETWORK_SINK_EMIT(emit_workload_class, WorkloadClassRecord, publish_workload_class)
FO_NETWORK_SINK_EMIT(emit_workload, WorkloadRecord, publish_workload)
FO_NETWORK_SINK_EMIT(emit_placement, PlacementRecord, publish_placement)
FO_NETWORK_SINK_EMIT(emit_migration, MigrationRecord, publish_migration)
FO_NETWORK_SINK_EMIT(emit_portability, PortabilityAssessment, publish_portability)

#undef FO_NETWORK_SINK_EMIT

Status NetworkSink::emit_capability(const ClusterId& cluster,
                                    ClusterGeneration cluster_generation,
                                    AcceleratorCapabilityGeneration capability_generation,
                                    CapabilitySet capabilities) {
  return absorb_status(publisher_->publish_capability(cluster, cluster_generation,
                                                      capability_generation, std::move(capabilities)),
                       "capability");
}

Status NetworkSink::emit_capacity(const ClusterId& cluster, ClusterGeneration cluster_generation,
                                  CapacityGeneration capacity_generation,
                                  std::vector<CapacityPool> pools) {
  return absorb_status(publisher_->publish_capacity(cluster, cluster_generation, capacity_generation,
                                                    std::move(pools)),
                       "capacity");
}

Status NetworkSink::emit_migration_stage(const MigrationId& migration,
                                         MigrationGeneration generation, MigrationStage stage,
                                         std::string detail) {
  return absorb_status(
      publisher_->publish_migration_stage(migration, generation, stage, std::move(detail)),
      "migration stage");
}

Status NetworkSink::emit_retire_cluster(const ClusterId& cluster, ClusterGeneration generation,
                                        std::string reason) {
  return absorb_status(publisher_->retire_cluster(cluster, generation, std::move(reason)),
                       "cluster retirement");
}

// ---------------------------------------------------------------------------
// SyntheticFederation
// ---------------------------------------------------------------------------

struct SyntheticFederation::Impl {
  SyntheticConfig config;
  PublicationSink* sink = nullptr;
  std::string last_error;
  EvidenceGeneration evidence_generation{1};
  FederationGeneration federation_generation{1};
  Expectations expectations;
  std::size_t cluster_count = 0;
  bool capability_fp8_removed = false;

  std::vector<SiteId> sites;
  std::vector<ClusterId> clusters;
  std::vector<AcceleratorClassId> accelerator_classes;
  std::vector<RuntimeId> runtimes;
  std::vector<BackendId> backends;
  std::vector<ArtifactId> artifacts;
  std::vector<WorkloadId> workloads;
  std::vector<WorkloadClassId> workload_classes;
  std::vector<PlacementId> placements;
  std::vector<MigrationId> migrations;
  std::vector<PolicyId> policies;
  std::vector<DomainId> domains;

  /// Every generated identifier carries the configured namespace prefix.
  [[nodiscard]] std::string prefixed(std::string base) const {
    return config.id_prefix + std::move(base);
  }
  [[nodiscard]] AcceleratorClassId accelerator_class(std::size_t family) const {
    return AcceleratorClassId::unchecked(prefixed("accel-class-" + index_suffix(family)));
  }
  [[nodiscard]] RuntimeId runtime(std::size_t family) const {
    return RuntimeId::unchecked(prefixed("runtime-" + std::string(kFamilies[family].family)));
  }
  [[nodiscard]] BackendId backend(std::size_t family) const {
    return BackendId::unchecked(prefixed("backend-" + std::string(kFamilies[family].family)));
  }
  [[nodiscard]] ClusterId cluster(std::size_t site, std::size_t index) const {
    return ClusterId::unchecked(prefixed("cluster-" + index_suffix(site) + "-" +
                                         index_suffix(index)));
  }
  [[nodiscard]] SiteId site(std::size_t index) const {
    return SiteId::unchecked(prefixed("site-" + index_suffix(index)));
  }
  [[nodiscard]] DomainId site_domain(std::size_t index) const {
    return DomainId::unchecked(prefixed("domain-site-" + index_suffix(index)));
  }
  [[nodiscard]] std::size_t family_of(std::size_t cluster_index) const {
    return cluster_index % config.accelerator_families;
  }

  Status fail_with(const Status& status) {
    if (!status.ok()) {
      last_error = status.error().to_string();
    }
    return status;
  }

  [[nodiscard]] ObservationStamp stamp() const {
    ObservationStamp result;
    result.publisher = config.publisher;
    result.publisher_boot = config.boot;
    result.evidence_generation = evidence_generation;
    result.observed_at = now_unix_nanos();
    result.precision = Precision::Exact;
    result.evidence_class = EvidenceClass::Synthetic;
    result.provenance = Provenance::SyntheticBackend;
    return result;
  }

  [[nodiscard]] CapabilitySet cluster_capabilities(std::size_t family, bool with_fp8) const {
    const FamilySpec& spec = kFamilies[family];
    CapabilitySet set;
    const EvidenceGeneration generation = evidence_generation;
    const auto put = [&set](CapabilityEntry entry) {
      const Status s = set.put(std::move(entry));
      (void)s;
    };
    put(make_entry(CapabilityKey::AcceleratorVendor, CapabilityValue::text(spec.vendor),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic accelerator vendor"));
    put(make_entry(CapabilityKey::AcceleratorFamily, CapabilityValue::text(spec.family),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic accelerator family"));
    put(make_entry(CapabilityKey::AcceleratorModel, CapabilityValue::text(spec.model),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic accelerator model"));
    put(make_entry(CapabilityKey::AcceleratorArchitecture, CapabilityValue::text(spec.architecture),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic accelerator architecture"));
    if (spec.compute_capability[0] != '\0') {
      put(make_entry(CapabilityKey::ComputeCapability,
                     CapabilityValue::text(spec.compute_capability), EvidenceClass::Synthetic,
                     Precision::Exact, generation, "synthetic compute capability"));
    }
    put(make_entry(CapabilityKey::MemoryBytes,
                   CapabilityValue::unsigned_integer(spec.memory_bytes), EvidenceClass::Synthetic,
                   Precision::Exact, generation, "synthetic per-device memory"));
    put(make_entry(CapabilityKey::MemoryBandwidthGbps,
                   CapabilityValue::unsigned_integer(spec.bandwidth_gbps), EvidenceClass::Synthetic,
                   Precision::Exact, generation, "synthetic memory bandwidth"));
    put(make_entry(CapabilityKey::PrecisionFp32, CapabilityValue::boolean(spec.fp32),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic fp32"));
    put(make_entry(CapabilityKey::PrecisionBf16, CapabilityValue::boolean(spec.bf16),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic bf16"));
    if (with_fp8) {
      put(make_entry(CapabilityKey::PrecisionFp8E4M3, CapabilityValue::boolean(spec.fp8),
                     EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic fp8"));
    }
    put(make_entry(CapabilityKey::RuntimeApi, CapabilityValue::text(spec.runtime_kind),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic runtime api"));
    put(make_entry(CapabilityKey::RuntimeVersion, CapabilityValue::text(spec.runtime_version),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic runtime version"));
    put(make_entry(CapabilityKey::RuntimeAbi, CapabilityValue::text(spec.runtime_abi),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic runtime abi"));
    put(make_entry(CapabilityKey::DriverVersion, CapabilityValue::text(spec.driver_version),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic driver version"));
    put(make_entry(CapabilityKey::DriverAbi, CapabilityValue::text(spec.driver_abi),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic driver abi"));
    put(make_entry(CapabilityKey::CompilerTarget, CapabilityValue::text(spec.compiler_target),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic compiler target"));
    put(make_entry(CapabilityKey::KernelFormat, CapabilityValue::text("cubin"),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic kernel format"));
    const Result<CapabilityValue> formats = CapabilityValue::text_set(
        {"cubin", "fatbin", "elf", "ptx", "safetensors", "torch-distributed-v2"});
    if (formats.ok()) {
      put(make_entry(CapabilityKey::ArtifactFormats, formats.value(), EvidenceClass::Synthetic,
                     Precision::Exact, generation, "synthetic artifact formats"));
    }
    put(make_entry(CapabilityKey::PartitionSupport, CapabilityValue::boolean(true),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic partition support"));
    const Result<CapabilityValue> partition_modes =
        CapabilityValue::text_set({spec.partition_mode, "full"});
    if (partition_modes.ok()) {
      put(make_entry(CapabilityKey::PartitionModes, partition_modes.value(),
                     EvidenceClass::Synthetic, Precision::Exact, generation,
                     "synthetic partition modes"));
    }
    put(make_entry(CapabilityKey::CollectiveSupport, CapabilityValue::boolean(true),
                   EvidenceClass::Synthetic, Precision::Exact, generation,
                   "synthetic collective support"));
    put(make_entry(CapabilityKey::RdmaCapability, CapabilityValue::boolean(family == 0),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic rdma"));
    put(make_entry(CapabilityKey::GpuDirectCapability, CapabilityValue::boolean(family == 0),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic gpudirect"));
    put(make_entry(CapabilityKey::OperatingSystem, CapabilityValue::text("linux"),
                   EvidenceClass::Synthetic, Precision::Exact, generation, "synthetic operating system"));
    const Result<CapabilityValue> isolation = CapabilityValue::text_set({"none", "process", "vm"});
    if (isolation.ok()) {
      put(make_entry(CapabilityKey::SecurityIsolation, isolation.value(), EvidenceClass::Synthetic,
                     Precision::Exact, generation, "synthetic isolation classes"));
    }
    const Result<CapabilityValue> serving =
        CapabilityValue::text_set({"text-generation-inference", "triton"});
    if (serving.ok()) {
      put(make_entry(CapabilityKey::InferenceServingBackend, serving.value(),
                     EvidenceClass::Synthetic, Precision::Exact, generation,
                     "synthetic serving backends"));
    }
    if (family == 1) {
      put(make_entry(CapabilityKey::OffloadCapability, CapabilityValue::boolean(false),
                     EvidenceClass::Synthetic, Precision::Exact, generation,
                     "synthetic offload capability"));
    }
    return set;
  }
};

SyntheticFederation::SyntheticFederation(SyntheticConfig config, PublicationSink& sink)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->sink = &sink;
  impl_->cluster_count = impl_->config.sites * impl_->config.clusters_per_site;
  for (std::size_t s = 0; s < impl_->config.sites; ++s) {
    impl_->sites.push_back(impl_->site(s));
    impl_->domains.push_back(impl_->site_domain(s));
  }
  for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
    const std::size_t site_index = i / impl_->config.clusters_per_site;
    const std::size_t local = i % impl_->config.clusters_per_site;
    impl_->clusters.push_back(impl_->cluster(site_index, local));
  }
  for (std::size_t f = 0; f < impl_->config.accelerator_families; ++f) {
    impl_->accelerator_classes.push_back(impl_->accelerator_class(f));
    impl_->runtimes.push_back(impl_->runtime(f));
    impl_->backends.push_back(impl_->backend(f));
  }
  const auto prefix = [this](std::string base) { return impl_->prefixed(std::move(base)); };
  impl_->artifacts = {ArtifactId::unchecked(prefix("artifact-train-sm90")),
                      ArtifactId::unchecked(prefix("artifact-infer-model")),
                      ArtifactId::unchecked(prefix("artifact-legacy-sm89"))};
  impl_->workload_classes = {WorkloadClassId::unchecked(prefix("wc-train")),
                             WorkloadClassId::unchecked(prefix("wc-infer")),
                             WorkloadClassId::unchecked(prefix("wc-legacy"))};
  impl_->workloads = {WorkloadId::unchecked(prefix("wl-train-1")),
                      WorkloadId::unchecked(prefix("wl-infer-1")),
                      WorkloadId::unchecked(prefix("wl-legacy-1"))};
  impl_->policies = {PolicyId::unchecked(prefix("policy-residency-a")),
                     PolicyId::unchecked(prefix("policy-open"))};
  impl_->placements = {PlacementId::unchecked(prefix("placement-1")),
                       PlacementId::unchecked(prefix("placement-2")),
                       PlacementId::unchecked(prefix("placement-3"))};
  impl_->migrations = {MigrationId::unchecked(prefix("migration-1")),
                       MigrationId::unchecked(prefix("migration-2"))};

  // Expected stranding computed from the same generator that produces the scenario.
  std::uint64_t nominal = 0;
  for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
    nominal += kFamilies[impl_->family_of(i)].devices_per_cluster;
  }
  nominal += kExtraFamilyTwoDevices;
  impl_->expectations.total_nominal_accelerators = nominal;
  impl_->expectations.placements_expected = impl_->placements.size();
  impl_->expectations.rejections_expected = 3;
  impl_->expectations.migrations_expected = impl_->migrations.size();
  impl_->expectations.portability_failures_expected = 2;
  impl_->expectations.fragmentation_required_group = 8;
  impl_->expectations.fragmentation_expected = true;
}

SyntheticFederation::~SyntheticFederation() = default;

const SyntheticConfig& SyntheticFederation::config() const noexcept { return impl_->config; }
const FederationId& SyntheticFederation::federation_id() const noexcept {
  return impl_->config.federation;
}
const std::vector<SiteId>& SyntheticFederation::site_ids() const noexcept { return impl_->sites; }
const std::vector<ClusterId>& SyntheticFederation::cluster_ids() const noexcept {
  return impl_->clusters;
}
const std::vector<AcceleratorClassId>& SyntheticFederation::accelerator_class_ids() const noexcept {
  return impl_->accelerator_classes;
}
const std::vector<RuntimeId>& SyntheticFederation::runtime_ids() const noexcept {
  return impl_->runtimes;
}
const std::vector<ArtifactId>& SyntheticFederation::artifact_ids() const noexcept {
  return impl_->artifacts;
}
const std::vector<WorkloadId>& SyntheticFederation::workload_ids() const noexcept {
  return impl_->workloads;
}
const std::vector<WorkloadClassId>& SyntheticFederation::workload_class_ids() const noexcept {
  return impl_->workload_classes;
}
const std::vector<PlacementId>& SyntheticFederation::placement_ids() const noexcept {
  return impl_->placements;
}
const std::vector<MigrationId>& SyntheticFederation::migration_ids() const noexcept {
  return impl_->migrations;
}
const std::vector<PolicyId>& SyntheticFederation::policy_ids() const noexcept {
  return impl_->policies;
}
const SyntheticFederation::Expectations& SyntheticFederation::expectations() const noexcept {
  return impl_->expectations;
}
const std::string& SyntheticFederation::last_error() const noexcept { return impl_->last_error; }

std::string SyntheticFederation::reproduction_record() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"seed", std::to_string(impl_->config.seed)});
  rows.push_back({"federation", impl_->config.federation.value()});
  rows.push_back({"publisher", impl_->config.publisher.value()});
  rows.push_back({"boot", impl_->config.boot.to_string()});
  rows.push_back({"sites", std::to_string(impl_->config.sites)});
  rows.push_back({"clusters_per_site", std::to_string(impl_->config.clusters_per_site)});
  rows.push_back({"accelerator_families", std::to_string(impl_->config.accelerator_families)});
  rows.push_back({"total_nominal_accelerators",
                  std::to_string(impl_->expectations.total_nominal_accelerators)});
  rows.push_back({"placements", std::to_string(impl_->placements.size())});
  rows.push_back({"migrations", std::to_string(impl_->migrations.size())});
  rows.push_back({"last_error", impl_->last_error.empty() ? "-" : impl_->last_error});
  return render_table({"reproduction", "value"}, rows, "");
}

Status SyntheticFederation::run(SyntheticStep step) {
  switch (step) {
    case SyntheticStep::Topology: {
      FederationRecord federation;
      federation.id = impl_->config.federation;
      federation.generation = FederationGeneration{1};
      federation.display_name = impl_->config.display_name;
      federation.coordinator_epoch = CoordinatorEpoch{1};
      federation.sites = impl_->sites;
      federation.clusters = impl_->clusters;
      federation.accelerator_classes = impl_->accelerator_classes;
      federation.runtimes = impl_->runtimes;
      federation.workload_classes = impl_->workload_classes;
      federation.policy = impl_->policies.front();
      federation.policy_generation = PolicyGeneration{1};
      federation.compatibility_generation = CompatibilityGeneration{1};
      federation.capacity_generation = CapacityGeneration{1};
      federation.topology_generation = TopologyGeneration{1};
      federation.evidence_generation = impl_->evidence_generation;
      federation.currentness = Currentness::Current;
      federation.stamp = impl_->stamp();
      const Status note = federation.evidence.add(Provenance::SyntheticBackend,
                                                  EvidenceClass::Synthetic, "synthetic-scenario",
                                                  Precision::Exact,
                                                  "synthetic federation topology; this is NOT a "
                                                  "physical multi-cluster federation");
      (void)note;
      Status status = impl_->sink->emit_federation(std::move(federation));
      if (!status.ok()) return impl_->fail_with(status);
      for (std::size_t s = 0; s < impl_->config.sites; ++s) {
        SiteRecord record;
        record.id = impl_->site(s);
        record.generation = SiteGeneration{1};
        record.federation = impl_->config.federation;
        record.region = "region-" + index_suffix(s);
        record.zone = "zone-" + index_suffix(s);
        record.failure_domain = "fd-" + index_suffix(s);
        for (std::size_t local = 0; local < impl_->config.clusters_per_site; ++local) {
          record.clusters.push_back(impl_->cluster(s, local));
        }
        record.policies = impl_->policies;
        record.currentness = Currentness::Current;
        record.stamp = impl_->stamp();
        status = impl_->sink->emit_site(std::move(record));
        if (!status.ok()) return impl_->fail_with(status);
      }
      // Cluster records are published twice on purpose. Referential integrity is checked
      // in both directions: a cluster may not name a domain that has not been registered,
      // and a domain may not name a cluster that has not been registered. The first
      // publication establishes the clusters without a domain, the second attaches the
      // domain once it exists. Both use the same cluster generation, so no capacity
      // evidence is invalidated by the second publication.
      const auto publish_clusters = [&](bool with_domain) {
        for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
          const std::size_t site_index = i / impl_->config.clusters_per_site;
          const std::size_t local = i % impl_->config.clusters_per_site;
          const std::size_t family = impl_->family_of(i);
          ClusterRecord cluster;
          cluster.id = impl_->cluster(site_index, local);
          cluster.generation = ClusterGeneration{1};
          cluster.epoch = ClusterEpoch{1};
          cluster.federation = impl_->config.federation;
          cluster.site = impl_->site(site_index);
          if (with_domain) {
            cluster.domain = impl_->site_domain(site_index);
          }
          cluster.failure_domain = "fd-" + index_suffix(site_index);
          cluster.zone = "zone-" + index_suffix(site_index);
          cluster.accelerator_classes = {impl_->accelerator_class(family)};
          cluster.runtimes = {impl_->runtime(family)};
          cluster.backends = {impl_->backend(family)};
          cluster.topology_generation = TopologyGeneration{1};
          cluster.capability_generation = AcceleratorCapabilityGeneration{1};
          cluster.capacity_generation = CapacityGeneration{1};
          cluster.runtime_generation = RuntimeGeneration{1};
          cluster.compatibility_generation = CompatibilityGeneration{1};
          cluster.evidence_generation = impl_->evidence_generation;
          cluster.readiness = Readiness::Ready;
          cluster.currentness = Currentness::Current;
          cluster.stamp = impl_->stamp();
          if (!with_domain) {
            const Status note = cluster.evidence.add(
                Provenance::SyntheticBackend, EvidenceClass::Synthetic, "synthetic-scenario",
                Precision::Exact,
                "synthetic cluster; this is NOT a physical cluster and the multi-cluster "
                "federation is SYNTHETIC");
            (void)note;
          }
          const Status emitted = impl_->sink->emit_cluster(std::move(cluster));
          if (!emitted.ok()) {
            return emitted;
          }
        }
        return Status::success();
      };

      status = publish_clusters(false);
      if (!status.ok()) return impl_->fail_with(status);

      for (std::size_t s = 0; s < impl_->config.sites; ++s) {
        DomainRecord domain;
        domain.id = impl_->site_domain(s);
        domain.kind = DomainKind::Site;
        domain.federation = impl_->config.federation;
        domain.site = impl_->site(s);
        for (std::size_t local = 0; local < impl_->config.clusters_per_site; ++local) {
          domain.clusters.push_back(impl_->cluster(s, local));
        }
        domain.topology_generation = TopologyGeneration{1};
        domain.currentness = Currentness::Current;
        domain.stamp = impl_->stamp();
        status = impl_->sink->emit_domain(std::move(domain));
        if (!status.ok()) return impl_->fail_with(status);
      }

      status = publish_clusters(true);
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::AcceleratorClasses: {
      for (std::size_t f = 0; f < impl_->config.accelerator_families; ++f) {
        const FamilySpec& spec = kFamilies[f];
        AcceleratorClassRecord record;
        record.id = impl_->accelerator_class(f);
        record.capability_generation = AcceleratorCapabilityGeneration{1};
        record.vendor = spec.vendor;
        record.family = spec.family;
        record.model = spec.model;
        record.architecture = spec.architecture;
        record.compute_capability = spec.compute_capability;
        record.memory_bytes_per_device = spec.memory_bytes;
        record.memory_bandwidth_gbps = spec.bandwidth_gbps;
        record.capabilities = impl_->cluster_capabilities(f, true);
        record.currentness = Currentness::Current;
        record.stamp = impl_->stamp();
        const Status status = impl_->sink->emit_accelerator_class(std::move(record));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }
    case SyntheticStep::Runtimes: {
      for (std::size_t f = 0; f < impl_->config.accelerator_families; ++f) {
        const FamilySpec& spec = kFamilies[f];
        RuntimeRecord record;
        record.id = impl_->runtime(f);
        record.generation = RuntimeGeneration{1};
        record.backend = impl_->backend(f);
        record.backend_generation = BackendGeneration{1};
        record.kind = runtime_kind_from(spec.runtime_kind);
        record.version = spec.runtime_version;
        record.abi = spec.runtime_abi;
        record.driver_version = spec.driver_version;
        record.driver_abi = spec.driver_abi;
        record.compiler_version = spec.runtime_version;
        record.currentness = Currentness::Current;
        record.stamp = impl_->stamp();
        const Status status = impl_->sink->emit_runtime(std::move(record));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }
    case SyntheticStep::Backends: {
      for (std::size_t f = 0; f < impl_->config.accelerator_families; ++f) {
        BackendRecord record;
        record.id = impl_->backend(f);
        record.generation = BackendGeneration{1};
        record.runtime = impl_->runtime(f);
        record.name = std::string("serving-") + kFamilies[f].family;
        record.version = "1.4.0";
        record.currentness = Currentness::Current;
        record.stamp = impl_->stamp();
        const Result<CapabilityValue> serving =
            CapabilityValue::text_set({"text-generation-inference"});
        if (serving.ok()) {
          CapabilityEntry entry;
          entry.key.key = CapabilityKey::InferenceServingBackend;
          entry.value = serving.value();
          entry.evidence_class = EvidenceClass::Synthetic;
          entry.precision = Precision::Exact;
          entry.generation = impl_->evidence_generation;
          const Status s = record.capabilities.put(std::move(entry));
          (void)s;
        }
        const Status status = impl_->sink->emit_backend(std::move(record));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }
    case SyntheticStep::Artifacts: {
      ArtifactRecord train;
      train.id = impl_->artifacts[0];
      train.generation = ArtifactGeneration{1};
      train.kind = ArtifactKind::DeviceImage;
      train.format = "cubin";
      train.kernel_format = "cubin";
      train.target_architectures = {"sm_90"};
      train.minimum_compute_capability = "9.0";
      train.size_bytes = 512ull * 1024 * 1024;
      train.runtime_abi = "cuda-12.4";
      train.driver_abi = "550.54";
      train.compiler_target = "sm_90";
      train.state_format = "torch-distributed-v2";
      train.required_memory_bytes = 64 * kGiB;
      train.currentness = Currentness::Current;
      train.stamp = impl_->stamp();
      Status status = impl_->sink->emit_artifact(std::move(train));
      if (!status.ok()) return impl_->fail_with(status);

      ArtifactRecord model;
      model.id = impl_->artifacts[1];
      model.generation = ArtifactGeneration{2};
      model.kind = ArtifactKind::ModelWeights;
      model.format = "safetensors";
      model.size_bytes = 24ull * 1024 * 1024 * 1024;
      model.required_memory_bytes = 24 * kGiB;
      model.currentness = Currentness::Current;
      model.stamp = impl_->stamp();
      status = impl_->sink->emit_artifact(std::move(model));
      if (!status.ok()) return impl_->fail_with(status);

      ArtifactRecord legacy;
      legacy.id = impl_->artifacts[2];
      legacy.generation = ArtifactGeneration{1};
      legacy.kind = ArtifactKind::ExecutableBinary;
      legacy.format = "cubin";
      legacy.kernel_format = "cubin";
      legacy.target_architectures = {"sm_89"};
      legacy.minimum_compute_capability = "8.9";
      legacy.runtime_abi = "cuda-12.4";
      legacy.driver_abi = "550.54";
      legacy.compiler_target = "sm_89";
      legacy.required_memory_bytes = 16 * kGiB;
      legacy.currentness = Currentness::Current;
      legacy.stamp = impl_->stamp();
      status = impl_->sink->emit_artifact(std::move(legacy));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }

    case SyntheticStep::WorkloadClasses: {
      const char* names[] = {"training", "inference", "legacy"};
      for (std::size_t i = 0; i < impl_->workload_classes.size(); ++i) {
        WorkloadClassRecord record;
        record.id = impl_->workload_classes[i];
        record.display_name = names[i];
        record.prefers_accelerators = true;
        record.currentness = Currentness::Current;
        record.stamp = impl_->stamp();
        const Status status = impl_->sink->emit_workload_class(std::move(record));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }
    case SyntheticStep::Policies: {
      PolicyRecord residency;
      residency.id = impl_->policies[0];
      residency.generation = PolicyGeneration{1};
      residency.name = "data-residency";
      for (std::size_t s = 0; s + 1 < impl_->config.sites; ++s) {
        residency.allowed_sites.push_back(impl_->site(s));
      }
      if (impl_->config.sites > 1) {
        residency.denied_sites.push_back(impl_->site(impl_->config.sites - 1));
      }
      residency.allow_cross_site = Tri::No;
      residency.data_residency = "region-restricted";
      residency.currentness = Currentness::Current;
      residency.stamp = impl_->stamp();
      Status status = impl_->sink->emit_policy(std::move(residency));
      if (!status.ok()) return impl_->fail_with(status);

      PolicyRecord open;
      open.id = impl_->policies[1];
      open.generation = PolicyGeneration{1};
      open.name = "open";
      open.allow_cross_site = Tri::Yes;
      open.currentness = Currentness::Current;
      open.stamp = impl_->stamp();
      status = impl_->sink->emit_policy(std::move(open));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::Domains: {
      // Domains are published with the topology; this step republishes a cluster-scoped
      // domain for the first cluster so that a federation-scoped requirement can be
      // evaluated against an explicit domain when one is registered.
      DomainRecord domain;
      domain.id = DomainId::unchecked(impl_->prefixed("domain-federation"));
      domain.kind = DomainKind::Federation;
      domain.federation = impl_->config.federation;
      domain.clusters = impl_->clusters;
      domain.topology_generation = TopologyGeneration{1};
      domain.currentness = Currentness::Current;
      domain.stamp = impl_->stamp();
      const Status status = impl_->sink->emit_domain(std::move(domain));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::Workloads: {
      WorkloadRecord train;
      train.id = impl_->workloads[0];
      train.generation = WorkloadGeneration{1};
      train.workload_class = impl_->workload_classes[0];
      train.artifact = impl_->artifacts[0];
      train.artifact_generation = ArtifactGeneration{1};
      train.required_accelerators = 8;
      train.required_memory_bytes_per_accelerator = 64 * kGiB;
      train.acceptable_accelerator_classes = {impl_->accelerator_class(0)};
      train.policy = impl_->policies[1];
      train.policy_generation = PolicyGeneration{1};
      train.required_domain_kind = DomainKind::Cluster;
      CapabilityRequirement fp8;
      fp8.key.key = CapabilityKey::PrecisionFp8E4M3;
      fp8.comparator = CapabilityComparator::Present;
      fp8.rationale = "training requires fp8";
      train.requirements.push_back(fp8);
      train.isolation_requirement = "process";
      train.portability_class = "trainer-v1";
      train.currentness = Currentness::Current;
      train.stamp = impl_->stamp();
      Status status = impl_->sink->emit_workload(std::move(train));
      if (!status.ok()) return impl_->fail_with(status);

      WorkloadRecord infer;
      infer.id = impl_->workloads[1];
      infer.generation = WorkloadGeneration{1};
      infer.workload_class = impl_->workload_classes[1];
      infer.artifact = impl_->artifacts[1];
      infer.artifact_generation = ArtifactGeneration{2};
      infer.required_accelerators = 2;
      infer.required_memory_bytes_per_accelerator = 24 * kGiB;
      infer.policy = impl_->policies[1];
      infer.policy_generation = PolicyGeneration{1};
      infer.required_domain_kind = DomainKind::Cluster;
      infer.requirements.push_back(fp8);
      infer.isolation_requirement = "process";
      infer.portability_class = "server-v1";
      infer.currentness = Currentness::Current;
      infer.stamp = impl_->stamp();
      status = impl_->sink->emit_workload(std::move(infer));
      if (!status.ok()) return impl_->fail_with(status);

      WorkloadRecord legacy;
      legacy.id = impl_->workloads[2];
      legacy.generation = WorkloadGeneration{1};
      legacy.workload_class = impl_->workload_classes[2];
      legacy.artifact = impl_->artifacts[2];
      legacy.artifact_generation = ArtifactGeneration{1};
      legacy.required_accelerators = 4;
      legacy.required_memory_bytes_per_accelerator = 16 * kGiB;
      legacy.acceptable_accelerator_classes = {impl_->accelerator_class(2)};
      legacy.policy = impl_->policies[0];
      legacy.policy_generation = PolicyGeneration{1};
      legacy.required_domain_kind = DomainKind::Cluster;
      CapabilityRequirement fp32;
      fp32.key.key = CapabilityKey::PrecisionFp32;
      fp32.comparator = CapabilityComparator::Present;
      fp32.rationale = "legacy kernels require fp32";
      legacy.requirements.push_back(fp32);
      legacy.portability_class = "legacy-v1";
      legacy.currentness = Currentness::Current;
      legacy.stamp = impl_->stamp();
      status = impl_->sink->emit_workload(std::move(legacy));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::Capabilities: {
      for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
        const std::size_t family = impl_->family_of(i);
        const std::size_t site_index = i / impl_->config.clusters_per_site;
        const std::size_t local = i % impl_->config.clusters_per_site;
        CapabilitySet capabilities = impl_->cluster_capabilities(family, true);
        const Status status = impl_->sink->emit_capability(
            impl_->cluster(site_index, local), ClusterGeneration{1},
            AcceleratorCapabilityGeneration{1}, std::move(capabilities));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }
    case SyntheticStep::Capacity: {
      for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
        const std::size_t family = impl_->family_of(i);
        const std::size_t site_index = i / impl_->config.clusters_per_site;
        const std::size_t local = i % impl_->config.clusters_per_site;
        std::vector<CapacityPool> pools;
        CapacityPool primary;
        primary.pool_id =
            ResourcePoolId::unchecked(impl_->prefixed("pool-" + index_suffix(i) + "-primary"));
        primary.kind = ResourceKind::Accelerator;
        primary.accelerator_class = impl_->accelerator_class(family);
        primary.generation = CapacityGeneration{1};
        const std::uint64_t devices = kFamilies[family].devices_per_cluster;
        // One cluster per site spends half of its devices on a reservation and one
        // cluster reports a draining device, so the ledger decomposition is exercised.
        const std::uint64_t allocated = (local == 1) ? 1 : 0;
        const std::uint64_t reserved = (local == 1) ? 2 : 0;
        const std::uint64_t draining = (local == 0 && site_index == 0) ? 1 : 0;
        const Result<CapacityLedger> ledger =
            CapacityLedger::from_components(devices, 0, allocated, reserved, draining, 0);
        if (!ledger.ok()) return impl_->fail_with(ledger.error());
        primary.ledger = ledger.value();
        primary.precision = Precision::Exact;
        primary.evidence_class = EvidenceClass::Synthetic;
        const Status primary_note = primary.evidence.add(
            Provenance::SyntheticBackend, EvidenceClass::Synthetic, "synthetic-scenario",
            Precision::Exact, "synthetic accelerator capacity ledger");
        (void)primary_note;
        pools.push_back(std::move(primary));

        CapacityPool memory;
        memory.pool_id =
            ResourcePoolId::unchecked(impl_->prefixed("pool-" + index_suffix(i) + "-memory"));
        memory.kind = ResourceKind::MemoryBytes;
        memory.accelerator_class = impl_->accelerator_class(family);
        memory.generation = CapacityGeneration{1};
        const Result<CapacityLedger> memory_ledger = CapacityLedger::from_components(
            devices * kFamilies[family].memory_bytes, 0, allocated * kFamilies[family].memory_bytes,
            0, 0, 0);
        if (!memory_ledger.ok()) return impl_->fail_with(memory_ledger.error());
        memory.ledger = memory_ledger.value();
        memory.precision = Precision::Exact;
        memory.evidence_class = EvidenceClass::Synthetic;
        pools.push_back(std::move(memory));

        if (i == 0 && impl_->config.accelerator_families > 2) {
          CapacityPool secondary;
          secondary.pool_id = ResourcePoolId::unchecked(impl_->prefixed("pool-0-secondary"));
          secondary.kind = ResourceKind::Accelerator;
          secondary.accelerator_class = impl_->accelerator_class(2);
          secondary.generation = CapacityGeneration{1};
          const Result<CapacityLedger> secondary_ledger =
              CapacityLedger::from_components(kExtraFamilyTwoDevices, 0, 0, 0, 0, 0);
          if (!secondary_ledger.ok()) return impl_->fail_with(secondary_ledger.error());
          secondary.ledger = secondary_ledger.value();
          secondary.precision = Precision::Exact;
          secondary.evidence_class = EvidenceClass::Synthetic;
          pools.push_back(std::move(secondary));
        }

        const Status status = impl_->sink->emit_capacity(
            impl_->cluster(site_index, local), ClusterGeneration{1}, CapacityGeneration{1},
            std::move(pools));
        if (!status.ok()) return impl_->fail_with(status);
      }
      return Status::success();
    }

    case SyntheticStep::Placements: {
      // A placement whose candidate set the scheduler exposed only partially.
      PlacementRecord first;
      first.id = impl_->placements[0];
      first.generation = PlacementGeneration{1};
      first.workload = impl_->workloads[0];
      first.workload_generation = WorkloadGeneration{1};
      first.workload_class = impl_->workload_classes[0];
      first.federation = impl_->config.federation;
      first.federation_generation = FederationGeneration{1};
      first.selected = impl_->cluster(0, 0);
      first.selected_generation = ClusterGeneration{1};
      first.selected_epoch = ClusterEpoch{1};
      first.selected_accelerator_class = impl_->accelerator_class(0);
      first.selected_accelerator_count = 4;
      first.selected_memory_bytes = 4 * 80 * kGiB;
      first.candidate_completeness = CandidateSetCompleteness::Partial;
      first.capacity_available = true;
      first.compatibility_constrained = Tri::Yes;
      first.capability_constrained = Tri::Yes;
      first.policy_generation = PolicyGeneration{1};
      first.capacity_generation = CapacityGeneration{1};
      first.topology_generation = TopologyGeneration{1};
      first.compatibility_generation = CompatibilityGeneration{1};
      first.currentness = Currentness::Current;
      first.stamp = impl_->stamp();

      CandidateObservation selected;
      selected.cluster = impl_->cluster(0, 0);
      selected.cluster_generation = ClusterGeneration{1};
      selected.cluster_epoch = ClusterEpoch{1};
      selected.status = CandidateStatus::Selected;
      selected.basis = ReasonBasis::Observed;
      selected.precision = Precision::Exact;
      selected.evidence_class = EvidenceClass::Synthetic;
      first.candidates.push_back(selected);

      CandidateObservation rejected_arch;
      rejected_arch.cluster = impl_->cluster(1, 0);
      rejected_arch.cluster_generation = ClusterGeneration{1};
      rejected_arch.cluster_epoch = ClusterEpoch{1};
      rejected_arch.status = CandidateStatus::Rejected;
      rejected_arch.reasons = rejection_bit(RejectionReason::ArtifactIncompatible) |
                              rejection_bit(RejectionReason::RuntimeIncompatible) |
                              rejection_bit(RejectionReason::CapabilityMissing);
      rejected_arch.basis = ReasonBasis::Observed;
      rejected_arch.precision = Precision::Exact;
      rejected_arch.evidence_class = EvidenceClass::Synthetic;
      rejected_arch.detail =
          "artifact targets sm_90; this cluster publishes gfx942 and no fp8 precision support";
      first.candidates.push_back(rejected_arch);

      CandidateObservation rejected_capacity;
      rejected_capacity.cluster = impl_->cluster(2, 0);
      rejected_capacity.cluster_generation = ClusterGeneration{1};
      rejected_capacity.cluster_epoch = ClusterEpoch{1};
      rejected_capacity.status = CandidateStatus::Rejected;
      rejected_capacity.reasons = rejection_bit(RejectionReason::InsufficientAccelerators);
      rejected_capacity.basis = ReasonBasis::Observed;
      rejected_capacity.precision = Precision::Exact;
      rejected_capacity.evidence_class = EvidenceClass::Synthetic;
      rejected_capacity.detail = "only 4 compatible accelerators were idle; the workload requires 8";
      first.candidates.push_back(rejected_capacity);

      Status status = impl_->sink->emit_placement(std::move(first));
      if (!status.ok()) return impl_->fail_with(status);

      // A placement where the scheduler exposed only the selected target.
      PlacementRecord second;
      second.id = impl_->placements[1];
      second.generation = PlacementGeneration{1};
      second.workload = impl_->workloads[1];
      second.workload_generation = WorkloadGeneration{1};
      second.workload_class = impl_->workload_classes[1];
      second.federation = impl_->config.federation;
      second.federation_generation = FederationGeneration{1};
      second.selected = impl_->cluster(0, 0);
      second.selected_generation = ClusterGeneration{1};
      second.selected_epoch = ClusterEpoch{1};
      second.selected_accelerator_class = impl_->accelerator_class(0);
      second.selected_accelerator_count = 2;
      second.candidate_completeness = CandidateSetCompleteness::SelectedOnly;
      second.capacity_available = true;
      second.policy_generation = PolicyGeneration{1};
      second.capacity_generation = CapacityGeneration{1};
      second.topology_generation = TopologyGeneration{1};
      second.compatibility_generation = CompatibilityGeneration{1};
      second.currentness = Currentness::Current;
      second.stamp = impl_->stamp();
      CandidateObservation only_selected;
      only_selected.cluster = impl_->cluster(0, 0);
      only_selected.cluster_generation = ClusterGeneration{1};
      only_selected.cluster_epoch = ClusterEpoch{1};
      only_selected.status = CandidateStatus::Selected;
      only_selected.basis = ReasonBasis::Observed;
      only_selected.precision = Precision::Exact;
      only_selected.evidence_class = EvidenceClass::Synthetic;
      second.candidates.push_back(only_selected);
      status = impl_->sink->emit_placement(std::move(second));
      if (!status.ok()) return impl_->fail_with(status);

      // A placement rejected on policy in the residency-restricted site.
      PlacementRecord third;
      third.id = impl_->placements[2];
      third.generation = PlacementGeneration{1};
      third.workload = impl_->workloads[2];
      third.workload_generation = WorkloadGeneration{1};
      third.workload_class = impl_->workload_classes[2];
      third.federation = impl_->config.federation;
      third.federation_generation = FederationGeneration{1};
      third.selected = impl_->cluster(0, 0);
      third.selected_generation = ClusterGeneration{1};
      third.selected_epoch = ClusterEpoch{1};
      third.selected_accelerator_class = impl_->accelerator_class(2);
      third.selected_accelerator_count = 4;
      third.candidate_completeness = CandidateSetCompleteness::Complete;
      third.capacity_available = true;
      third.policy_constrained = Tri::Yes;
      third.policy_generation = PolicyGeneration{1};
      third.capacity_generation = CapacityGeneration{1};
      third.topology_generation = TopologyGeneration{1};
      third.compatibility_generation = CompatibilityGeneration{1};
      third.fallback_required = true;
      third.fallback_detail = "the preferred site is excluded by the data-residency policy";
      third.currentness = Currentness::Current;
      third.stamp = impl_->stamp();
      CandidateObservation third_selected;
      third_selected.cluster = impl_->cluster(0, 0);
      third_selected.cluster_generation = ClusterGeneration{1};
      third_selected.status = CandidateStatus::Selected;
      third_selected.basis = ReasonBasis::Observed;
      third_selected.precision = Precision::Exact;
      third_selected.evidence_class = EvidenceClass::Synthetic;
      third.candidates.push_back(third_selected);
      CandidateObservation third_rejected;
      third_rejected.cluster = impl_->cluster(impl_->config.sites - 1, 0);
      third_rejected.cluster_generation = ClusterGeneration{1};
      third_rejected.status = CandidateStatus::Rejected;
      third_rejected.reasons = rejection_bit(RejectionReason::PolicyRejected) |
                               rejection_bit(RejectionReason::SiteRestriction);
      third_rejected.basis = ReasonBasis::Observed;
      third_rejected.precision = Precision::Exact;
      third_rejected.evidence_class = EvidenceClass::Synthetic;
      third_rejected.detail = "policy-residency-a excludes this site";
      third.candidates.push_back(third_rejected);
      status = impl_->sink->emit_placement(std::move(third));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::Migrations: {
      MigrationRecord first;
      first.id = impl_->migrations[0];
      first.generation = MigrationGeneration{1};
      first.workload = impl_->workloads[1];
      first.workload_generation = WorkloadGeneration{1};
      first.workload_class = impl_->workload_classes[1];
      first.federation = impl_->config.federation;
      first.federation_generation = FederationGeneration{1};
      first.source = impl_->cluster(0, 0);
      first.source_generation = ClusterGeneration{1};
      first.source_epoch = ClusterEpoch{1};
      first.destination = impl_->cluster(1, 0);
      first.destination_generation = ClusterGeneration{1};
      first.destination_epoch = ClusterEpoch{1};
      first.artifact = impl_->artifacts[1];
      first.artifact_generation = ArtifactGeneration{2};
      first.source_runtime = impl_->runtime(0);
      first.source_runtime_generation = RuntimeGeneration{1};
      first.destination_runtime = impl_->runtime(1);
      first.destination_runtime_generation = RuntimeGeneration{1};
      first.compatibility_generation = CompatibilityGeneration{1};
      first.policy_generation = PolicyGeneration{1};
      first.stage = MigrationStage::Planned;
      first.outcome = MigrationOutcome::InProgress;
      first.reason = "rebalance";
      first.reason_observed = true;
      first.state_transfer_known = true;
      first.state_transfer_bytes = 24ull * 1024 * 1024 * 1024;
      first.downtime_known = true;
      first.downtime_micros = 45000;
      first.currentness = Currentness::Current;
      first.stamp = impl_->stamp();
      MigrationStageEvent planned;
      planned.generation = MigrationGeneration{1};
      planned.stage = MigrationStage::Planned;
      planned.sequence = Sequence{1};
      planned.observed_at = first.stamp.observed_at;
      planned.precision = Precision::Exact;
      planned.evidence_class = EvidenceClass::Synthetic;
      first.stage_events.push_back(planned);
      Status status = impl_->sink->emit_migration(std::move(first));
      if (!status.ok()) return impl_->fail_with(status);

      const MigrationStage stages[] = {
          MigrationStage::SourceQuiescing, MigrationStage::StateCaptured,
          MigrationStage::TransferStarted, MigrationStage::TransferComplete,
          MigrationStage::DestinationPrepared, MigrationStage::RestoreStarted,
          MigrationStage::RestoreComplete, MigrationStage::RevalidationRequired,
          MigrationStage::Committed};
      for (const MigrationStage stage : stages) {
        status = impl_->sink->emit_migration_stage(impl_->migrations[0], MigrationGeneration{1},
                                                   stage, "synthetic migration stage");
        if (!status.ok()) return impl_->fail_with(status);
      }

      // A second migration for the same workload that supersedes the first. The first
      // migration therefore stops being current, and late events bearing its generation
      // must be refused.
      MigrationRecord second;
      second.id = impl_->migrations[1];
      second.generation = MigrationGeneration{2};
      second.workload = impl_->workloads[1];
      second.workload_generation = WorkloadGeneration{1};
      second.workload_class = impl_->workload_classes[1];
      second.federation = impl_->config.federation;
      second.federation_generation = FederationGeneration{1};
      second.supersedes = impl_->migrations[0];
      second.supersedes_generation = MigrationGeneration{1};
      second.source = impl_->cluster(1, 0);
      second.source_generation = ClusterGeneration{1};
      second.source_epoch = ClusterEpoch{1};
      second.destination = impl_->cluster(2, 0);
      second.destination_generation = ClusterGeneration{1};
      second.destination_epoch = ClusterEpoch{1};
      second.artifact = impl_->artifacts[1];
      second.artifact_generation = ArtifactGeneration{2};
      second.source_runtime = impl_->runtime(1);
      second.source_runtime_generation = RuntimeGeneration{1};
      second.destination_runtime = impl_->runtime(0);
      second.destination_runtime_generation = RuntimeGeneration{1};
      second.compatibility_generation = CompatibilityGeneration{1};
      second.policy_generation = PolicyGeneration{1};
      second.stage = MigrationStage::Planned;
      second.outcome = MigrationOutcome::InProgress;
      second.currentness = Currentness::Current;
      second.stamp = impl_->stamp();
      MigrationStageEvent second_planned;
      second_planned.generation = MigrationGeneration{2};
      second_planned.stage = MigrationStage::Planned;
      second_planned.sequence = Sequence{1};
      second_planned.observed_at = second.stamp.observed_at;
      second_planned.precision = Precision::Exact;
      second_planned.evidence_class = EvidenceClass::Synthetic;
      second.stage_events.push_back(second_planned);
      status = impl_->sink->emit_migration(std::move(second));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }

    case SyntheticStep::Portability: {
      // The synthetic backend evaluates portability with the production assessor against
      // the same records it published, so the published assessment and a later
      // re-evaluation agree by construction.
      const auto build = [&](const WorkloadId& workload_id, const ClusterId& destination_id,
                             AcceleratorClassRecord& class_storage, RuntimeRecord& runtime_storage,
                             ClusterRecord& cluster_storage, PolicyRecord& policy_storage,
                             bool& have_class, bool& have_runtime, bool& have_cluster,
                             bool& have_policy) {
        PortabilityInputs inputs;
        inputs.federation = impl_->config.federation;
        inputs.federation_generation = FederationGeneration{1};
        inputs.compatibility_generation = CompatibilityGeneration{1};
        inputs.policy_generation = PolicyGeneration{1};

        WorkloadRecord workload;
        if (workload_id == impl_->workloads[0]) {
          workload.id = impl_->workloads[0];
          workload.generation = WorkloadGeneration{1};
          workload.workload_class = impl_->workload_classes[0];
          workload.artifact = impl_->artifacts[0];
          workload.artifact_generation = ArtifactGeneration{1};
          workload.required_accelerators = 8;
          workload.required_memory_bytes_per_accelerator = 64 * kGiB;
          workload.isolation_requirement = "process";
          CapabilityRequirement requirement;
          requirement.key.key = CapabilityKey::PrecisionFp8E4M3;
          requirement.comparator = CapabilityComparator::Present;
          workload.requirements.push_back(requirement);
        } else if (workload_id == impl_->workloads[1]) {
          workload.id = impl_->workloads[1];
          workload.generation = WorkloadGeneration{1};
          workload.workload_class = impl_->workload_classes[1];
          workload.artifact = impl_->artifacts[1];
          workload.artifact_generation = ArtifactGeneration{2};
          workload.required_accelerators = 2;
          workload.required_memory_bytes_per_accelerator = 24 * kGiB;
          workload.isolation_requirement = "process";
          CapabilityRequirement requirement;
          requirement.key.key = CapabilityKey::PrecisionFp8E4M3;
          requirement.comparator = CapabilityComparator::Present;
          workload.requirements.push_back(requirement);
        } else {
          workload.id = impl_->workloads[2];
          workload.generation = WorkloadGeneration{1};
          workload.workload_class = impl_->workload_classes[2];
          workload.artifact = impl_->artifacts[2];
          workload.artifact_generation = ArtifactGeneration{1};
          workload.required_accelerators = 4;
          workload.required_memory_bytes_per_accelerator = 16 * kGiB;
          workload.isolation_requirement = "process";
        }
        inputs.workload = &workload;

        ArtifactRecord artifact;
        if (workload.artifact == impl_->artifacts[0]) {
          artifact.id = impl_->artifacts[0];
          artifact.generation = ArtifactGeneration{1};
          artifact.kind = ArtifactKind::DeviceImage;
          artifact.format = "cubin";
          artifact.kernel_format = "cubin";
          artifact.target_architectures = {"sm_90"};
          artifact.minimum_compute_capability = "9.0";
          artifact.runtime_abi = "cuda-12.4";
          artifact.driver_abi = "550.54";
          artifact.compiler_target = "sm_90";
          artifact.state_format = "torch-distributed-v2";
        } else if (workload.artifact == impl_->artifacts[1]) {
          artifact.id = impl_->artifacts[1];
          artifact.generation = ArtifactGeneration{2};
          artifact.kind = ArtifactKind::ModelWeights;
          artifact.format = "safetensors";
          artifact.required_memory_bytes = 24 * kGiB;
        } else {
          artifact.id = impl_->artifacts[2];
          artifact.generation = ArtifactGeneration{1};
          artifact.kind = ArtifactKind::ExecutableBinary;
          artifact.format = "cubin";
          artifact.kernel_format = "cubin";
          artifact.target_architectures = {"sm_89"};
          artifact.runtime_abi = "cuda-12.4";
          artifact.compiler_target = "sm_89";
        }
        inputs.artifact = &artifact;

        const ClusterId source_id = impl_->cluster(0, 0);
        const ClusterId destination_id_local = destination_id;
        cluster_storage.id = source_id;
        cluster_storage.generation = ClusterGeneration{1};
        cluster_storage.epoch = ClusterEpoch{1};
        cluster_storage.federation = impl_->config.federation;
        cluster_storage.site = impl_->site(0);
        cluster_storage.readiness = Readiness::Ready;
        cluster_storage.currentness = Currentness::Current;
        cluster_storage.runtimes = {impl_->runtime(0)};
        cluster_storage.accelerator_classes = {impl_->accelerator_class(0)};
        have_cluster = true;
        inputs.source_cluster = &cluster_storage;

        runtime_storage.id = impl_->runtime(0);
        runtime_storage.generation = RuntimeGeneration{1};
        runtime_storage.kind = RuntimeKind::Cuda;
        runtime_storage.abi = "cuda-12.4";
        runtime_storage.version = "12.4.1";
        runtime_storage.driver_abi = "550.54";
        runtime_storage.currentness = Currentness::Current;
        have_runtime = true;
        inputs.source_runtime = &runtime_storage;

        class_storage.id = impl_->accelerator_class(0);
        class_storage.capability_generation = AcceleratorCapabilityGeneration{1};
        class_storage.architecture = "sm_90";
        class_storage.memory_bytes_per_device = 80 * kGiB;
        class_storage.capabilities = impl_->cluster_capabilities(0, true);
        class_storage.currentness = Currentness::Current;
        have_class = true;
        inputs.source_accelerator_class = &class_storage;

        // Destination cluster.
        std::size_t destination_index = 0;
        for (std::size_t i = 0; i < impl_->cluster_count; ++i) {
          const std::size_t site_index = i / impl_->config.clusters_per_site;
          const std::size_t local = i % impl_->config.clusters_per_site;
          if (impl_->cluster(site_index, local) == destination_id_local) {
            destination_index = i;
            break;
          }
        }
        const std::size_t destination_family = impl_->family_of(destination_index);
        const std::size_t destination_site = destination_index / impl_->config.clusters_per_site;
        ClusterRecord destination_cluster;
        destination_cluster.id = destination_id_local;
        destination_cluster.generation = ClusterGeneration{1};
        destination_cluster.epoch = ClusterEpoch{1};
        destination_cluster.federation = impl_->config.federation;
        destination_cluster.site = impl_->site(destination_site);
        destination_cluster.readiness = Readiness::Ready;
        destination_cluster.currentness = Currentness::Current;
        destination_cluster.runtimes = {impl_->runtime(destination_family)};
        destination_cluster.accelerator_classes = {impl_->accelerator_class(destination_family)};
        destination_cluster.capabilities = impl_->cluster_capabilities(destination_family, true);

        AcceleratorClassRecord destination_class;
        destination_class.id = impl_->accelerator_class(destination_family);
        destination_class.capability_generation = AcceleratorCapabilityGeneration{1};
        destination_class.architecture = kFamilies[destination_family].architecture;
        destination_class.compute_capability = kFamilies[destination_family].compute_capability;
        destination_class.memory_bytes_per_device = kFamilies[destination_family].memory_bytes;
        destination_class.capabilities = impl_->cluster_capabilities(destination_family, true);

        RuntimeRecord destination_runtime;
        destination_runtime.id = impl_->runtime(destination_family);
        destination_runtime.generation = RuntimeGeneration{1};
        destination_runtime.kind = runtime_kind_from(kFamilies[destination_family].runtime_kind);
        destination_runtime.abi = kFamilies[destination_family].runtime_abi;
        destination_runtime.version = kFamilies[destination_family].runtime_version;
        destination_runtime.driver_abi = kFamilies[destination_family].driver_abi;

        CapabilitySet effective = impl_->cluster_capabilities(destination_family, true);
        {
          const Status s = [&] {
            for (const CapabilityEntry& entry : destination_class.capabilities.entries()) {
              const Status put = effective.put(entry);
              (void)put;
            }
            for (const CapabilityEntry& entry : destination_runtime.capabilities.entries()) {
              const Status put = effective.put(entry);
              (void)put;
            }
            return Status::success();
          }();
          (void)s;
        }
        inputs.destination_capabilities = &effective;

        // The destination cluster must outlive the assessment call.
        static thread_local ClusterRecord destination_storage;
        destination_storage = destination_cluster;
        inputs.destination_cluster = &destination_storage;
        static thread_local AcceleratorClassRecord destination_class_storage;
        destination_class_storage = destination_class;
        inputs.destination_accelerator_class = &destination_class_storage;
        static thread_local RuntimeRecord destination_runtime_storage;
        destination_runtime_storage = destination_runtime;
        inputs.destination_runtime = &destination_runtime_storage;

        policy_storage.id = impl_->policies[0];
        policy_storage.generation = PolicyGeneration{1};
        if (destination_site + 1 >= impl_->config.sites && impl_->config.sites > 1) {
          policy_storage.denied_sites.push_back(impl_->site(destination_site));
        }
        policy_storage.currentness = Currentness::Current;
        have_policy = true;
        inputs.policy = &policy_storage;

        static thread_local CapabilitySet effective_storage;
        effective_storage = effective;
        inputs.destination_capabilities = &effective_storage;

        return assess_portability(inputs);
      };

      AcceleratorClassRecord class_storage;
      RuntimeRecord runtime_storage;
      ClusterRecord cluster_storage;
      PolicyRecord policy_storage;
      bool have_class = false;
      bool have_runtime = false;
      bool have_cluster = false;
      bool have_policy = false;

      PortabilityAssessment runtime_change =
          build(impl_->workloads[1], impl_->cluster(1, 0), class_storage, runtime_storage,
                cluster_storage, policy_storage, have_class, have_runtime, have_cluster, have_policy);
      runtime_change.stamp = impl_->stamp();
      runtime_change.currentness = Currentness::Current;
      Status status = impl_->sink->emit_portability(std::move(runtime_change));
      if (!status.ok()) return impl_->fail_with(status);

      PortabilityAssessment policy_blocked = build(
          impl_->workloads[2], impl_->cluster(impl_->config.sites - 1, 0), class_storage,
          runtime_storage, cluster_storage, policy_storage, have_class, have_runtime, have_cluster,
          have_policy);
      policy_blocked.stamp = impl_->stamp();
      policy_blocked.currentness = Currentness::Current;
      status = impl_->sink->emit_portability(std::move(policy_blocked));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::CapabilityChange: {
      // The accelerator capability generation advances on the first cluster and fp8
      // support is withdrawn, which is exactly the change that must invalidate prior
      // stranded-capacity conclusions.
      impl_->capability_fp8_removed = true;
      impl_->evidence_generation = impl_->evidence_generation.next();
      CapabilitySet capabilities = impl_->cluster_capabilities(0, false);
      const Status status = impl_->sink->emit_capability(
          impl_->cluster(0, 0), ClusterGeneration{1}, AcceleratorCapabilityGeneration{2},
          std::move(capabilities));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::CapacityChange: {
      std::vector<CapacityPool> pools;
      CapacityPool primary;
      primary.pool_id = ResourcePoolId::unchecked(impl_->prefixed("pool-0-primary"));
      primary.kind = ResourceKind::Accelerator;
      primary.accelerator_class = impl_->accelerator_class(0);
      primary.generation = CapacityGeneration{2};
      const Result<CapacityLedger> ledger =
          CapacityLedger::from_components(8, 0, 2, 0, 0, 0);
      if (!ledger.ok()) return impl_->fail_with(ledger.error());
      primary.ledger = ledger.value();
      primary.precision = Precision::Exact;
      primary.evidence_class = EvidenceClass::Synthetic;
      pools.push_back(std::move(primary));
      const Status status = impl_->sink->emit_capacity(
          impl_->cluster(0, 0), ClusterGeneration{1}, CapacityGeneration{2}, std::move(pools));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::ClusterJoin: {
      ClusterRecord record;
      record.id = ClusterId::unchecked(impl_->prefixed("cluster-join-1"));
      record.generation = ClusterGeneration{1};
      record.epoch = ClusterEpoch{1};
      record.federation = impl_->config.federation;
      record.site = impl_->site(0);
      record.domain = impl_->site_domain(0);
      record.failure_domain = "fd-0";
      record.zone = "zone-0";
      record.accelerator_classes = {impl_->accelerator_class(0)};
      record.runtimes = {impl_->runtime(0)};
      record.backends = {impl_->backend(0)};
      record.topology_generation = TopologyGeneration{2};
      record.capability_generation = AcceleratorCapabilityGeneration{1};
      record.capacity_generation = CapacityGeneration{1};
      record.runtime_generation = RuntimeGeneration{1};
      record.readiness = Readiness::Ready;
      record.currentness = Currentness::Current;
      record.stamp = impl_->stamp();
      Status status = impl_->sink->emit_cluster(std::move(record));
      if (!status.ok()) return impl_->fail_with(status);
      status = impl_->sink->emit_capability(ClusterId::unchecked(impl_->prefixed("cluster-join-1")),
                                            ClusterGeneration{1},
                                            AcceleratorCapabilityGeneration{1},
                                            impl_->cluster_capabilities(0, true));
      if (!status.ok()) return impl_->fail_with(status);
      std::vector<CapacityPool> pools;
      CapacityPool primary;
      primary.pool_id = ResourcePoolId::unchecked(impl_->prefixed("pool-join-1-primary"));
      primary.kind = ResourceKind::Accelerator;
      primary.accelerator_class = impl_->accelerator_class(0);
      primary.generation = CapacityGeneration{1};
      const Result<CapacityLedger> ledger = CapacityLedger::from_components(8, 0, 0, 0, 0, 0);
      if (!ledger.ok()) return impl_->fail_with(ledger.error());
      primary.ledger = ledger.value();
      primary.precision = Precision::Exact;
      primary.evidence_class = EvidenceClass::Synthetic;
      pools.push_back(std::move(primary));
      status = impl_->sink->emit_capacity(ClusterId::unchecked(impl_->prefixed("cluster-join-1")),
                                          ClusterGeneration{1}, CapacityGeneration{1},
                                          std::move(pools));
      if (!status.ok()) return impl_->fail_with(status);
      return Status::success();
    }
    case SyntheticStep::ClusterLeave: {
      const ClusterId leaving = impl_->cluster(impl_->config.sites - 1, 0);
      const Status status =
          impl_->sink->emit_retire_cluster(leaving, ClusterGeneration{1},
                                           "planned decommission in the synthetic scenario");
      return impl_->fail_with(status);
    }
    case SyntheticStep::StaleEvidence: {
      // Advancing a cluster's generation without republishing its capacity leaves the
      // cluster registered while its dynamic evidence is not current. This is what a
      // restart or a publisher death looks like from the observation side.
      const std::size_t index = impl_->cluster_count > 1 ? impl_->cluster_count - 2 : 0;
      const std::size_t site_index = index / impl_->config.clusters_per_site;
      const std::size_t local = index % impl_->config.clusters_per_site;
      ClusterRecord record;
      record.id = impl_->cluster(site_index, local);
      record.generation = ClusterGeneration{2};
      record.epoch = ClusterEpoch{2};
      record.federation = impl_->config.federation;
      record.site = impl_->site(site_index);
      record.domain = impl_->site_domain(site_index);
      record.failure_domain = "fd-" + index_suffix(site_index);
      record.accelerator_classes = {impl_->accelerator_class(impl_->family_of(index))};
      record.runtimes = {impl_->runtime(impl_->family_of(index))};
      record.topology_generation = TopologyGeneration{2};
      record.capability_generation = AcceleratorCapabilityGeneration{1};
      record.capacity_generation = CapacityGeneration{1};
      record.runtime_generation = RuntimeGeneration{1};
      record.readiness = Readiness::Ready;
      record.currentness = Currentness::Current;
      record.stamp = impl_->stamp();
      const Status note = record.evidence.add(
          Provenance::SyntheticBackend, EvidenceClass::Synthetic, "synthetic-scenario",
          Precision::Exact,
          "cluster incarnation restarted under a new epoch; its capacity evidence must be "
          "republished before it can be considered current");
      (void)note;
      const Status status = impl_->sink->emit_cluster(std::move(record));
      return impl_->fail_with(status);
    }
    case SyntheticStep::RepairTopology: {
      // A new cluster with eight compatible accelerators makes the previously
      // fragmented requirement satisfiable in one legal placement domain.
      ClusterRecord record;
      record.id = ClusterId::unchecked(impl_->prefixed("cluster-repair-1"));
      record.generation = ClusterGeneration{1};
      record.epoch = ClusterEpoch{1};
      record.federation = impl_->config.federation;
      record.site = impl_->site(0);
      record.domain = impl_->site_domain(0);
      record.failure_domain = "fd-0";
      record.zone = "zone-0";
      record.accelerator_classes = {impl_->accelerator_class(0)};
      record.runtimes = {impl_->runtime(0)};
      record.backends = {impl_->backend(0)};
      record.topology_generation = TopologyGeneration{3};
      record.capability_generation = AcceleratorCapabilityGeneration{1};
      record.capacity_generation = CapacityGeneration{1};
      record.runtime_generation = RuntimeGeneration{1};
      record.readiness = Readiness::Ready;
      record.currentness = Currentness::Current;
      record.stamp = impl_->stamp();
      Status status = impl_->sink->emit_cluster(std::move(record));
      if (!status.ok()) return impl_->fail_with(status);
      status = impl_->sink->emit_capability(ClusterId::unchecked(impl_->prefixed("cluster-repair-1")),
                                            ClusterGeneration{1},
                                            AcceleratorCapabilityGeneration{1},
                                            impl_->cluster_capabilities(0, true));
      if (!status.ok()) return impl_->fail_with(status);
      std::vector<CapacityPool> pools;
      CapacityPool primary;
      primary.pool_id = ResourcePoolId::unchecked(impl_->prefixed("pool-repair-1-primary"));
      primary.kind = ResourceKind::Accelerator;
      primary.accelerator_class = impl_->accelerator_class(0);
      primary.generation = CapacityGeneration{1};
      const Result<CapacityLedger> ledger = CapacityLedger::from_components(8, 0, 0, 0, 0, 0);
      if (!ledger.ok()) return impl_->fail_with(ledger.error());
      primary.ledger = ledger.value();
      primary.precision = Precision::Exact;
      primary.evidence_class = EvidenceClass::Synthetic;
      pools.push_back(std::move(primary));
      status = impl_->sink->emit_capacity(ClusterId::unchecked(impl_->prefixed("cluster-repair-1")),
                                          ClusterGeneration{1}, CapacityGeneration{1},
                                          std::move(pools));
      return impl_->fail_with(status);
    }
  }
  return fail(ErrorCode::Internal, "unhandled synthetic step");
}

Status SyntheticFederation::run_all() {
  const Status registered = impl_->sink->register_self();
  if (!registered.ok()) {
    return impl_->fail_with(registered);
  }
  const SyntheticStep order[] = {
      SyntheticStep::AcceleratorClasses, SyntheticStep::Runtimes,
      SyntheticStep::Backends,           SyntheticStep::Topology,
      SyntheticStep::Policies,           SyntheticStep::Domains,
      SyntheticStep::Artifacts,          SyntheticStep::WorkloadClasses,
      SyntheticStep::Workloads,          SyntheticStep::Capabilities,
      SyntheticStep::Capacity,           SyntheticStep::Placements,
      SyntheticStep::Migrations,         SyntheticStep::Portability,
  };
  for (const SyntheticStep step : order) {
    const Status status = run(step);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

}  // namespace fo
