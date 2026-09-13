// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// fo-benchmarks - completed-operation measurements over the Federation Observatory API.
//
// The runtime measured here is OBSERVATIONAL. It never chooses a placement, never moves a
// workload and never owns policy. Every record this harness publishes is synthetic
// evidence (fo::ObservatorySink, fo::Provenance::SyntheticBackend) attributed to a named
// publisher; the numbers below are the cost of ingesting and analyzing observations, not
// the cost of making scheduling decisions, which belongs to the upstream scheduler.
//
// Method
//  * one fixture per federation size, built through fo::ObservatorySink - the same sink
//    the deterministic synthetic backend publishes through - so every record travels the
//    production validation, generation-fencing and digest path;
//  * fixture construction is excluded from every measurement;
//  * each operation is warmed up once and then timed over a printed number of completed,
//    applied operations with std::chrono::steady_clock;
//  * an operation that the runtime refuses aborts the run instead of producing a number
//    for work that did not happen;
//  * only measured values are printed; nothing is modelled, extrapolated or invented.
//
// Usage
//   fo-benchmarks [--scale <1|10|100|1000|10000>] [--only <substring>]... [--help]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "federation_observatory/capacity.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/snapshot.hpp"
#include "federation_observatory/synthetic.hpp"
#include "federation_observatory/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Fixture shape. Fixed across every scale so that only the cluster count varies.
constexpr std::uint64_t kDevicesPerCluster = 4;
constexpr std::uint64_t kDeviceMemoryBytes = 80ull * 1024ull * 1024ull * 1024ull;
constexpr std::size_t kPlacementCandidateTarget = 4;
constexpr std::size_t kCapabilitiesPerCluster = 8;

// ---------------------------------------------------------------------------
// Failures. A refused publication or analysis means the measurement is not a
// measurement of anything, so the harness stops rather than reporting it.
// ---------------------------------------------------------------------------

struct BenchError {
  std::string detail;
};

[[noreturn]] void bench_fail(std::string detail) { throw BenchError{std::move(detail)}; }

void require_ok(const fo::Status& status, std::string_view what) {
  if (!status.ok()) {
    bench_fail(std::string(what) + " was refused: " + status.to_string());
  }
}

template <class T>
void require_ok(const fo::Result<T>& result, std::string_view what) {
  if (!result.ok()) {
    bench_fail(std::string(what) + " was refused: " + result.error().to_string());
  }
}

// ---------------------------------------------------------------------------
// Deterministic identifiers and records
// ---------------------------------------------------------------------------

struct Ids {
  fo::FederationId federation = fo::FederationId::unchecked("bench-fed");
  fo::PublisherId publisher = fo::PublisherId::unchecked("bench-publisher");
  fo::SiteId site = fo::SiteId::unchecked("bench-site");
  fo::AcceleratorClassId accelerator_class =
      fo::AcceleratorClassId::unchecked("bench-accelerator-class");
  fo::RuntimeId runtime = fo::RuntimeId::unchecked("bench-runtime");
  fo::ArtifactId artifact = fo::ArtifactId::unchecked("bench-artifact");
  fo::WorkloadClassId workload_class = fo::WorkloadClassId::unchecked("bench-workload-class");
  fo::WorkloadId workload = fo::WorkloadId::unchecked("bench-workload");
};

const Ids& ids() {
  static const Ids instance;
  return instance;
}

fo::ClusterId cluster_id(std::size_t index) {
  return fo::ClusterId::unchecked("bench-cluster-" + std::to_string(index));
}

fo::PlacementId placement_id(const std::string& suffix) {
  return fo::PlacementId::unchecked("bench-placement-" + suffix);
}

fo::MigrationId migration_id(std::size_t index) {
  return fo::MigrationId::unchecked("bench-migration-" + std::to_string(index));
}

void put_capability(fo::CapabilitySet& set, fo::CapabilityKey key, fo::CapabilityValue value) {
  fo::CapabilityEntry entry;
  entry.key.key = key;
  entry.value = std::move(value);
  entry.precision = fo::Precision::Exact;
  entry.evidence_class = fo::EvidenceClass::Synthetic;
  entry.generation = fo::EvidenceGeneration{1};
  require_ok(set.put(std::move(entry)), "CapabilitySet::put");
}

/// A modest capability set: the kind of publication a cluster agent sends on a
/// capability refresh. Kept identical at every scale.
fo::CapabilitySet make_capabilities() {
  fo::CapabilitySet set;
  put_capability(set, fo::CapabilityKey::AcceleratorArchitecture,
                 fo::CapabilityValue::text("sm_90"));
  put_capability(set, fo::CapabilityKey::AcceleratorVendor, fo::CapabilityValue::text("nvidia"));
  put_capability(set, fo::CapabilityKey::AcceleratorModel,
                 fo::CapabilityValue::text("bench-model"));
  put_capability(set, fo::CapabilityKey::MemoryBytes,
                 fo::CapabilityValue::unsigned_integer(kDeviceMemoryBytes));
  put_capability(set, fo::CapabilityKey::MemoryBandwidthGbps,
                 fo::CapabilityValue::unsigned_integer(3350));
  put_capability(set, fo::CapabilityKey::PrecisionFp32, fo::CapabilityValue::boolean(true));
  put_capability(set, fo::CapabilityKey::PrecisionBf16, fo::CapabilityValue::boolean(true));
  put_capability(set, fo::CapabilityKey::RuntimeAbi, fo::CapabilityValue::text("cuda-12.4"));
  return set;
}

fo::FederationRecord make_federation(std::size_t clusters) {
  fo::FederationRecord record;
  record.id = ids().federation;
  record.generation = fo::FederationGeneration{1};
  record.display_name = "benchmark-federation";
  record.coordinator_epoch = fo::CoordinatorEpoch{1};
  record.sites = {ids().site};
  record.accelerator_classes = {ids().accelerator_class};
  record.runtimes = {ids().runtime};
  record.workload_classes = {ids().workload_class};
  record.compatibility_generation = fo::CompatibilityGeneration{1};
  record.capacity_generation = fo::CapacityGeneration{1};
  record.topology_generation = fo::TopologyGeneration{1};
  record.evidence_generation = fo::EvidenceGeneration{1};
  record.clusters.reserve(clusters);
  for (std::size_t index = 0; index < clusters; ++index) {
    record.clusters.push_back(cluster_id(index));
  }
  return record;
}

/// The site declares its region, zone and failure domain but not its membership: a site
/// record may carry at most kMaxClusterPerSite (4096) cluster references and the largest
/// benchmark federation holds more clusters than that. Membership is observed from the
/// clusters themselves, each of which names this site.
fo::SiteRecord make_site() {
  fo::SiteRecord record;
  record.id = ids().site;
  record.generation = fo::SiteGeneration{1};
  record.federation = ids().federation;
  record.region = "bench-region";
  record.zone = "bench-zone";
  record.failure_domain = "bench-fd";
  return record;
}

fo::AcceleratorClassRecord make_accelerator_class() {
  fo::AcceleratorClassRecord record;
  record.id = ids().accelerator_class;
  record.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  record.vendor = "nvidia";
  record.family = "bench-family";
  record.model = "bench-model";
  record.architecture = "sm_90";
  record.compute_capability = "9.0";
  record.memory_bytes_per_device = kDeviceMemoryBytes;
  record.memory_bandwidth_gbps = 3350;
  record.capabilities = make_capabilities();
  return record;
}

fo::RuntimeRecord make_runtime() {
  fo::RuntimeRecord record;
  record.id = ids().runtime;
  record.generation = fo::RuntimeGeneration{1};
  record.kind = fo::RuntimeKind::Cuda;
  record.version = "12.4.1";
  record.abi = "cuda-12.4";
  record.driver_version = "550.54.15";
  record.driver_abi = "550.54";
  record.compiler_version = "12.4.1";
  return record;
}

fo::ArtifactRecord make_artifact() {
  fo::ArtifactRecord record;
  record.id = ids().artifact;
  record.generation = fo::ArtifactGeneration{1};
  record.kind = fo::ArtifactKind::DeviceImage;
  record.format = "cubin";
  record.kernel_format = "cubin";
  record.target_architectures = {"sm_90"};
  record.minimum_compute_capability = "9.0";
  record.size_bytes = 512ull * 1024ull * 1024ull;
  record.runtime_abi = "cuda-12.4";
  record.driver_abi = "550.54";
  record.compiler_target = "sm_90";
  record.required_memory_bytes = kDeviceMemoryBytes;
  return record;
}

fo::WorkloadClassRecord make_workload_class() {
  fo::WorkloadClassRecord record;
  record.id = ids().workload_class;
  record.display_name = "benchmark-class";
  record.prefers_accelerators = true;
  return record;
}

fo::WorkloadRecord make_workload() {
  fo::WorkloadRecord record;
  record.id = ids().workload;
  record.generation = fo::WorkloadGeneration{1};
  record.workload_class = ids().workload_class;
  record.artifact = ids().artifact;
  record.artifact_generation = fo::ArtifactGeneration{1};
  record.required_accelerators = static_cast<std::uint32_t>(kDevicesPerCluster);
  record.required_memory_bytes_per_accelerator = kDeviceMemoryBytes;
  record.acceptable_accelerator_classes = {ids().accelerator_class};
  record.required_domain_kind = fo::DomainKind::Cluster;
  record.isolation_requirement = "process";
  record.portability_class = "benchmark-v1";
  fo::CapabilityRequirement requirement;
  requirement.key.key = fo::CapabilityKey::PrecisionBf16;
  requirement.comparator = fo::CapabilityComparator::Present;
  requirement.rationale = "benchmark workload requires bf16";
  record.requirements.push_back(std::move(requirement));
  return record;
}

fo::ClusterRecord make_cluster(const fo::ClusterId& id) {
  fo::ClusterRecord record;
  record.id = id;
  record.generation = fo::ClusterGeneration{1};
  record.epoch = fo::ClusterEpoch{1};
  record.federation = ids().federation;
  record.site = ids().site;
  record.failure_domain = "bench-fd";
  record.zone = "bench-zone";
  record.accelerator_classes = {ids().accelerator_class};
  record.runtimes = {ids().runtime};
  record.topology_generation = fo::TopologyGeneration{1};
  // Capability and capacity generations are deliberately unset at registration: the
  // runtime refuses a dynamic publication whose generation equals the stored one, so a
  // joining cluster publishes its first capability and capacity evidence under
  // generation 1 only when registration did not claim one.
  record.runtime_generation = fo::RuntimeGeneration{1};
  record.compatibility_generation = fo::CompatibilityGeneration{1};
  record.evidence_generation = fo::EvidenceGeneration{1};
  record.readiness = fo::Readiness::Ready;
  record.currentness = fo::Currentness::Current;
  return record;
}

std::vector<fo::CapacityPool> make_capacity_pools(fo::CapacityGeneration generation) {
  fo::CapacityPool pool;
  pool.pool_id = fo::ResourcePoolId::unchecked("bench-pool-accelerators");
  pool.kind = fo::ResourceKind::Accelerator;
  pool.accelerator_class = ids().accelerator_class;
  pool.generation = generation;
  const fo::Result<fo::CapacityLedger> ledger =
      fo::CapacityLedger::from_components(kDevicesPerCluster, 0, 0, 0, 0, 0);
  require_ok(ledger, "CapacityLedger::from_components");
  pool.ledger = ledger.value();
  pool.precision = fo::Precision::Exact;
  pool.evidence_class = fo::EvidenceClass::Synthetic;
  std::vector<fo::CapacityPool> pools;
  pools.push_back(std::move(pool));
  return pools;
}

/// A placement as the upstream scheduler reported it: one selected cluster and
/// rejected candidates with structured reasons. Candidate count is bounded by the
/// number of registered clusters, because a candidate that names an unregistered
/// cluster is refused rather than observed.
fo::PlacementRecord make_placement(const fo::PlacementId& id, std::size_t candidates) {
  fo::PlacementRecord record;
  record.id = id;
  record.generation = fo::PlacementGeneration{1};
  record.workload = ids().workload;
  record.workload_generation = fo::WorkloadGeneration{1};
  record.workload_class = ids().workload_class;
  record.federation = ids().federation;
  record.federation_generation = fo::FederationGeneration{1};
  record.selected = cluster_id(0);
  record.selected_generation = fo::ClusterGeneration{1};
  record.selected_epoch = fo::ClusterEpoch{1};
  record.selected_accelerator_class = ids().accelerator_class;
  record.selected_accelerator_count = static_cast<std::uint32_t>(kDevicesPerCluster);
  record.selected_memory_bytes = kDevicesPerCluster * kDeviceMemoryBytes;
  record.policy_generation = fo::PolicyGeneration{1};
  record.capacity_generation = fo::CapacityGeneration{1};
  record.topology_generation = fo::TopologyGeneration{1};
  record.compatibility_generation = fo::CompatibilityGeneration{1};
  record.capacity_available = true;
  record.candidate_completeness = candidates > 1 ? fo::CandidateSetCompleteness::Partial
                                                 : fo::CandidateSetCompleteness::SelectedOnly;

  fo::CandidateObservation selected;
  selected.cluster = cluster_id(0);
  selected.cluster_generation = fo::ClusterGeneration{1};
  selected.cluster_epoch = fo::ClusterEpoch{1};
  selected.status = fo::CandidateStatus::Selected;
  selected.basis = fo::ReasonBasis::Observed;
  selected.precision = fo::Precision::Exact;
  selected.evidence_class = fo::EvidenceClass::Synthetic;
  selected.detail = "reported by the upstream scheduler as the selected target";
  record.candidates.push_back(std::move(selected));

  for (std::size_t index = 1; index < candidates; ++index) {
    fo::CandidateObservation rejected;
    rejected.cluster = cluster_id(index);
    rejected.cluster_generation = fo::ClusterGeneration{1};
    rejected.cluster_epoch = fo::ClusterEpoch{1};
    rejected.status = fo::CandidateStatus::Rejected;
    rejected.reasons = fo::rejection_bit(fo::RejectionReason::InsufficientAccelerators) |
                       fo::rejection_bit(fo::RejectionReason::RuntimeIncompatible);
    rejected.basis = fo::ReasonBasis::Observed;
    rejected.precision = fo::Precision::Exact;
    rejected.evidence_class = fo::EvidenceClass::Synthetic;
    rejected.detail = "reported by the upstream scheduler as an evaluated, rejected candidate";
    record.candidates.push_back(std::move(rejected));
  }
  return record;
}

/// An observed migration of a workload between two clusters. The decision belongs to
/// the upstream scheduler; this record only describes what was seen.
fo::MigrationRecord make_migration(const fo::MigrationId& id) {
  fo::MigrationRecord record;
  record.id = id;
  record.generation = fo::MigrationGeneration{1};
  record.workload = ids().workload;
  record.workload_generation = fo::WorkloadGeneration{1};
  record.workload_class = ids().workload_class;
  record.federation = ids().federation;
  record.federation_generation = fo::FederationGeneration{1};
  record.source = cluster_id(0);
  record.source_generation = fo::ClusterGeneration{1};
  record.source_epoch = fo::ClusterEpoch{1};
  record.destination = cluster_id(1);
  record.destination_generation = fo::ClusterGeneration{1};
  record.destination_epoch = fo::ClusterEpoch{1};
  record.artifact = ids().artifact;
  record.artifact_generation = fo::ArtifactGeneration{1};
  record.source_runtime = ids().runtime;
  record.source_runtime_generation = fo::RuntimeGeneration{1};
  record.destination_runtime = ids().runtime;
  record.destination_runtime_generation = fo::RuntimeGeneration{1};
  record.compatibility_generation = fo::CompatibilityGeneration{1};
  record.policy_generation = fo::PolicyGeneration{1};
  record.capacity_generation = fo::CapacityGeneration{1};
  record.topology_generation = fo::TopologyGeneration{1};
  record.stage = fo::MigrationStage::Planned;
  record.outcome = fo::MigrationOutcome::InProgress;
  record.reason = "observed upstream rebalance decision (the runtime does not choose it)";
  record.reason_observed = true;

  fo::MigrationStageEvent planned;
  planned.generation = fo::MigrationGeneration{1};
  planned.stage = fo::MigrationStage::Planned;
  planned.sequence = fo::Sequence{1};
  planned.precision = fo::Precision::Exact;
  planned.evidence_class = fo::EvidenceClass::Synthetic;
  planned.detail = "upstream scheduler announced the migration";
  record.stage_events.push_back(std::move(planned));
  return record;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct Fixture {
  fo::FederationObservatory observatory;
  std::unique_ptr<fo::ObservatorySink> sink;
  std::size_t clusters = 0;
  std::size_t publications = 0;
};

std::unique_ptr<Fixture> build_fixture(std::size_t clusters, bool with_dynamic_evidence,
                                       std::size_t candidate_count) {
  auto fixture = std::make_unique<Fixture>();
  fixture->clusters = clusters;
  fixture->sink = std::make_unique<fo::ObservatorySink>(
      fixture->observatory, ids().publisher, fo::BootGeneration{1}, ids().federation);

  require_ok(fixture->sink->register_self(), "register_self");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_federation(make_federation(clusters)), "emit_federation");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_site(make_site()), "emit_site");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_accelerator_class(make_accelerator_class()),
             "emit_accelerator_class");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_runtime(make_runtime()), "emit_runtime");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_artifact(make_artifact()), "emit_artifact");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_workload_class(make_workload_class()), "emit_workload_class");
  fixture->publications += 1;
  require_ok(fixture->sink->emit_workload(make_workload()), "emit_workload");
  fixture->publications += 1;

  for (std::size_t index = 0; index < clusters; ++index) {
    require_ok(fixture->sink->emit_cluster(make_cluster(cluster_id(index))), "emit_cluster");
    fixture->publications += 1;
  }
  if (!with_dynamic_evidence) {
    return fixture;
  }

  const fo::CapabilitySet capabilities = make_capabilities();
  if (capabilities.size() != kCapabilitiesPerCluster) {
    bench_fail("capability fixture drifted from the size the operation label states");
  }
  for (std::size_t index = 0; index < clusters; ++index) {
    require_ok(fixture->sink->emit_capability(cluster_id(index), fo::ClusterGeneration{1},
                                              fo::AcceleratorCapabilityGeneration{1}, capabilities),
               "emit_capability");
    fixture->publications += 1;
  }
  for (std::size_t index = 0; index < clusters; ++index) {
    require_ok(fixture->sink->emit_capacity(cluster_id(index), fo::ClusterGeneration{1},
                                            fo::CapacityGeneration{1},
                                            make_capacity_pools(fo::CapacityGeneration{1})),
               "emit_capacity");
    fixture->publications += 1;
  }

  // Published last so that "the last placement" is this record when the explanation
  // measurement runs.
  require_ok(fixture->sink->emit_placement(make_placement(placement_id("fixture"), candidate_count)),
             "emit_placement");
  fixture->publications += 1;
  return fixture;
}

void verify_fixture(const Fixture& fixture, std::size_t candidates) {
  const fo::SnapshotHandle handle = fixture.observatory.snapshot();
  if (handle->health.clusters_total != fixture.clusters) {
    bench_fail("fixture snapshot holds " + std::to_string(handle->health.clusters_total) +
               " clusters, expected " + std::to_string(fixture.clusters));
  }
  if (handle->placements.size() != 1) {
    bench_fail("fixture expects exactly one placement record, found " +
               std::to_string(handle->placements.size()));
  }
  if (handle->placements.front().candidates.size() != candidates) {
    bench_fail("fixture placement does not carry the expected candidate count");
  }
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string format_fixed(double value, int decimals) {
  char buffer[64];
  const int written = std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  if (written <= 0) {
    return std::string();
  }
  return std::string(buffer);
}

struct Row {
  std::string label;
  std::size_t order = 0;
  std::size_t scale = 0;
  std::size_t iterations = 0;
  double total_ms = 0.0;
  double us_per_operation = 0.0;
  bool ran = true;
  /// Why an operation could not be measured. Never rendered as a number.
  std::string note;
};

/// Warm up once, then time \p iterations complete operations. \p body receives an
/// iteration index; index 0 is the warm-up, so the measured repetitions never repeat
/// a warm-up payload.
template <class Body>
Row measure(std::string label, std::size_t order, std::size_t scale, std::size_t iterations,
            Body&& body) {
  body(std::size_t{0});
  const Clock::time_point start = Clock::now();
  for (std::size_t index = 1; index <= iterations; ++index) {
    body(index);
  }
  const Clock::time_point stop = Clock::now();

  Row row;
  row.label = std::move(label);
  row.order = order;
  row.scale = scale;
  row.iterations = iterations;
  row.total_ms = std::chrono::duration<double, std::milli>(stop - start).count();
  row.us_per_operation = row.total_ms * 1000.0 / static_cast<double>(iterations);
  return row;
}

Row skipped_row(std::string label, std::size_t order, std::size_t scale, std::string note) {
  Row row;
  row.label = std::move(label);
  row.order = order;
  row.scale = scale;
  row.ran = false;
  row.note = std::move(note);
  return row;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

enum class Operation : std::size_t {
  RegisterCluster = 0,
  PublishCapability,
  PublishCapacity,
  PublishPlacement,
  SnapshotCreation,
  ExplainPlacement,
  StrandedCapacity,
  Fragmentation,
  MismatchAnalysis,
  CompatibilityLookup,
  MigrationUpdate,
  PortabilityAnalysis,
  Persistence,
  Count,
};

struct OperationInfo {
  Operation op;
  std::string_view key;
  std::string_view label;
};

constexpr OperationInfo kOperations[] = {
    {Operation::RegisterCluster, "register_cluster",
     "cluster publication (register_cluster)"},
    {Operation::PublishCapability, "publish_capability",
     "capability update (publish_capability, 8 capabilities)"},
    {Operation::PublishCapacity, "publish_capacity",
     "capacity update (publish_capacity, 1 accelerator pool per cluster)"},
    {Operation::PublishPlacement, "publish_placement",
     "placement ingestion (publish_placement, 4 candidates)"},
    {Operation::SnapshotCreation, "snapshot",
     "snapshot creation (FederationObservatory::snapshot)"},
    {Operation::ExplainPlacement, "explain_placement",
     "placement explanation (explain_placement on the last placement)"},
    {Operation::StrandedCapacity, "stranded_capacity",
     "stranded capacity (stranded_capacity, workload class)"},
    {Operation::Fragmentation, "fragmentation",
     "fragmentation (fragmentation, workload class)"},
    {Operation::MismatchAnalysis, "mismatch_analysis",
     "capability mismatch (mismatch_analysis, federation)"},
    {Operation::CompatibilityLookup, "compatibility",
     "compatibility lookup (compatibility, cluster 0 + workload)"},
    {Operation::MigrationUpdate, "publish_migration",
     "migration update (publish_migration + 2 migration stages)"},
    {Operation::PortabilityAnalysis, "evaluate_portability",
     "portability analysis (evaluate_portability, destination cluster)"},
    {Operation::Persistence, "save_state",
     "persistence (save_state + load_state, one temporary file)"},
};

const OperationInfo& info_of(Operation op) {
  for (const OperationInfo& info : kOperations) {
    if (info.op == op) {
      return info;
    }
  }
  return kOperations[0];
}

std::string_view label_of(Operation op) { return info_of(op).label; }

std::size_t order_of(Operation op) { return static_cast<std::size_t>(op); }

std::string lower_copy(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Scale plan. Iteration counts are fixed per scale so a run is reproducible apart
// from the timings themselves; every count is printed for the reader to judge.
// ---------------------------------------------------------------------------

struct ScalePlan {
  std::size_t clusters;
  std::size_t query_iterations;
  std::size_t light_iterations;
  std::size_t history_iterations;
  std::size_t persistence_iterations;
};

constexpr ScalePlan kPlans[] = {
    {1, 2000, 2000, 500, 200},
    {10, 2000, 2000, 500, 100},
    {100, 500, 1000, 200, 50},
    {1000, 100, 500, 100, 10},
    {10000, 20, 200, 20, 2},
};

/// Cluster registration adds clusters to the fixture, so its repetition count is
/// bounded: at most a 25% increase of the federation, never fewer than 16
/// registrations and never more than 2500.
std::size_t registration_iterations(std::size_t clusters) {
  const std::size_t quarter = clusters / 4;
  if (quarter < 16) {
    return 16;
  }
  if (quarter > 2500) {
    return 2500;
  }
  return quarter;
}

class TempStateFile {
 public:
  TempStateFile() {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    const std::filesystem::path base = error ? std::filesystem::path(".") : directory;
    path_ = (base / "fo-benchmarks-state.bin").string();
  }
  ~TempStateFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  TempStateFile(const TempStateFile&) = delete;
  TempStateFile& operator=(const TempStateFile&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

class Selection {
 public:
  void add(std::string filter) { filters_.push_back(lower_copy(filter)); }

  [[nodiscard]] bool empty() const noexcept { return filters_.empty(); }

  [[nodiscard]] bool wants(Operation op) const {
    if (filters_.empty()) {
      return true;
    }
    const std::string haystack =
        lower_copy(info_of(op).key) + " " + lower_copy(info_of(op).label);
    for (const std::string& filter : filters_) {
      if (haystack.find(filter) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool wants_any_dynamic() const {
    for (const OperationInfo& info : kOperations) {
      if (info.op != Operation::RegisterCluster && wants(info.op)) {
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<std::string> filters_;
};

// ---------------------------------------------------------------------------
// One scale
// ---------------------------------------------------------------------------

struct ScaleReport {
  std::size_t clusters = 0;
  std::size_t fixture_publications = 0;
  double fixture_setup_ms = 0.0;
  std::vector<Row> rows;
};

ScaleReport run_scale(const ScalePlan& plan, const Selection& selection) {
  ScaleReport report;
  report.clusters = plan.clusters;
  const std::size_t clusters = plan.clusters;
  const std::size_t candidates = std::min<std::size_t>(kPlacementCandidateTarget, clusters);
  const std::string placement_label =
      std::string("placement ingestion (publish_placement, ") + std::to_string(candidates) +
      " candidates)";

  const Clock::time_point setup_start = Clock::now();
  std::unique_ptr<Fixture> registration_fixture;
  std::unique_ptr<Fixture> fixture;
  if (selection.wants(Operation::RegisterCluster)) {
    registration_fixture = build_fixture(clusters, false, candidates);
    report.fixture_publications += registration_fixture->publications;
  }
  if (selection.wants_any_dynamic()) {
    fixture = build_fixture(clusters, true, candidates);
    report.fixture_publications += fixture->publications;
    verify_fixture(*fixture, candidates);
  }
  report.fixture_setup_ms = elapsed_ms(setup_start);

  if (registration_fixture) {
    const std::size_t iterations = registration_iterations(clusters);
    report.rows.push_back(measure(
        std::string(label_of(Operation::RegisterCluster)), order_of(Operation::RegisterCluster),
        clusters, iterations, [&](std::size_t index) {
          require_ok(registration_fixture->sink->emit_cluster(make_cluster(cluster_id(clusters + index))),
                     "register_cluster");
        }));
  }

  if (fixture) {
    // Queries first: they read the fixture exactly as built (one pool of four idle
    // devices per cluster, one placement). Ingest measurements below deliberately run
    // afterwards so that they cannot perturb the state the queries are measured on.
    if (selection.wants(Operation::SnapshotCreation)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::SnapshotCreation)), order_of(Operation::SnapshotCreation),
          clusters, plan.query_iterations, [&](std::size_t) {
            const fo::SnapshotHandle handle = fixture->observatory.snapshot();
            if (handle->health.clusters_total != clusters) {
              bench_fail("snapshot did not include every registered cluster");
            }
          }));
    }

    if (selection.wants(Operation::ExplainPlacement)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::ExplainPlacement)), order_of(Operation::ExplainPlacement),
          clusters, plan.query_iterations, [&](std::size_t) {
            require_ok(fixture->observatory.explain_placement(placement_id("fixture")),
                       "explain_placement");
          }));
    }

    if (selection.wants(Operation::StrandedCapacity)) {
      fo::StrandedCapacityRequest request;
      request.federation = ids().federation;
      request.workload_class = ids().workload_class;
      request.kind = fo::ResourceKind::Accelerator;
      report.rows.push_back(measure(
          std::string(label_of(Operation::StrandedCapacity)), order_of(Operation::StrandedCapacity),
          clusters, plan.query_iterations, [&](std::size_t) {
            require_ok(fixture->observatory.stranded_capacity(request), "stranded_capacity");
          }));
    }

    if (selection.wants(Operation::Fragmentation)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::Fragmentation)), order_of(Operation::Fragmentation),
          clusters, plan.query_iterations, [&](std::size_t) {
            require_ok(fixture->observatory.fragmentation(ids().federation, ids().workload_class,
                                                          fo::ResourceKind::Accelerator),
                       "fragmentation");
          }));
    }

    if (selection.wants(Operation::MismatchAnalysis)) {
      fo::MismatchAnalysisRequest request;
      request.federation = ids().federation;
      request.kind = fo::ResourceKind::Accelerator;
      report.rows.push_back(measure(
          std::string(label_of(Operation::MismatchAnalysis)), order_of(Operation::MismatchAnalysis),
          clusters, plan.query_iterations, [&](std::size_t) {
            require_ok(fixture->observatory.mismatch_analysis(request), "mismatch_analysis");
          }));
    }

    if (selection.wants(Operation::CompatibilityLookup)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::CompatibilityLookup)),
          order_of(Operation::CompatibilityLookup), clusters, plan.query_iterations,
          [&](std::size_t) {
            require_ok(fixture->observatory.compatibility(cluster_id(0), ids().workload),
                       "compatibility");
          }));
    }

    if (selection.wants(Operation::PortabilityAnalysis)) {
      const fo::ClusterId destination = cluster_id(clusters > 1 ? 1 : 0);
      report.rows.push_back(measure(
          std::string(label_of(Operation::PortabilityAnalysis)),
          order_of(Operation::PortabilityAnalysis), clusters, plan.query_iterations,
          [&](std::size_t) {
            require_ok(fixture->observatory.evaluate_portability(ids().workload, destination),
                       "evaluate_portability");
          }));
    }

    if (selection.wants(Operation::PublishCapability)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::PublishCapability)), order_of(Operation::PublishCapability),
          clusters, plan.light_iterations, [&](std::size_t index) {
            require_ok(fixture->sink->emit_capability(
                           cluster_id(0), fo::ClusterGeneration{1},
                           fo::AcceleratorCapabilityGeneration{2 + index}, make_capabilities()),
                       "publish_capability");
          }));
    }

    if (selection.wants(Operation::PublishCapacity)) {
      report.rows.push_back(measure(
          std::string(label_of(Operation::PublishCapacity)), order_of(Operation::PublishCapacity),
          clusters, plan.light_iterations, [&](std::size_t index) {
            const fo::CapacityGeneration generation{2 + index};
            require_ok(fixture->sink->emit_capacity(cluster_id(0), fo::ClusterGeneration{1},
                                                    generation, make_capacity_pools(generation)),
                       "publish_capacity");
          }));
    }

    if (selection.wants(Operation::PublishPlacement)) {
      report.rows.push_back(measure(
          placement_label, order_of(Operation::PublishPlacement), clusters, plan.history_iterations,
          [&](std::size_t index) {
            require_ok(fixture->sink->emit_placement(
                           make_placement(placement_id(std::to_string(index)), candidates)),
                       "publish_placement");
          }));
    }

    if (selection.wants(Operation::MigrationUpdate)) {
      if (clusters < 2) {
        report.rows.push_back(skipped_row(
            std::string(label_of(Operation::MigrationUpdate)), order_of(Operation::MigrationUpdate),
            clusters,
            "a migration record requires distinct source and destination clusters and this fixture "
            "holds one cluster"));
      } else {
        report.rows.push_back(measure(
            std::string(label_of(Operation::MigrationUpdate)), order_of(Operation::MigrationUpdate),
            clusters, plan.history_iterations, [&](std::size_t index) {
              const fo::MigrationId migration = migration_id(index);
              require_ok(fixture->sink->emit_migration(make_migration(migration)),
                         "publish_migration");
              require_ok(fixture->sink->emit_migration_stage(
                             migration, fo::MigrationGeneration{1},
                             fo::MigrationStage::SourceQuiescing,
                             "upstream scheduler reported the source quiescing"),
                         "publish_migration_stage");
              require_ok(fixture->sink->emit_migration_stage(
                             migration, fo::MigrationGeneration{1},
                             fo::MigrationStage::StateCaptured,
                             "upstream scheduler reported the state capture"),
                         "publish_migration_stage");
            }));
      }
    }

    if (selection.wants(Operation::Persistence)) {
      const std::string persistence_label(label_of(Operation::Persistence));
      TempStateFile state_file;
      // Persistence is the one measured path that the runtime may refuse outright for a
      // live observation state; a refusal is a fact about the runtime, so it is reported
      // as a fact and never as a duration.
      const fo::Status saved = fixture->observatory.save_state(state_file.path());
      const fo::Status loaded =
          saved.ok() ? fixture->observatory.load_state(state_file.path()) : saved;
      if (!saved.ok() || !loaded.ok()) {
        report.rows.push_back(skipped_row(
            persistence_label, order_of(Operation::Persistence), clusters,
            std::string(saved.ok() ? "load_state" : "save_state") + " was refused: " +
                (saved.ok() ? loaded.to_string() : saved.to_string())));
      } else {
        report.rows.push_back(measure(
            persistence_label, order_of(Operation::Persistence), clusters,
            plan.persistence_iterations, [&](std::size_t) {
              require_ok(fixture->observatory.save_state(state_file.path()), "save_state");
              require_ok(fixture->observatory.load_state(state_file.path()), "load_state");
            }));
      }
    }
  }

  std::stable_sort(report.rows.begin(), report.rows.end(),
                   [](const Row& a, const Row& b) { return a.order < b.order; });
  return report;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void print_scale_report(const ScaleReport& report) {
  std::cout << "\n"
            << "scale " << report.clusters << " clusters: fixture of " << report.fixture_publications
            << " publications through fo::ObservatorySink built in "
            << format_fixed(report.fixture_setup_ms, 3) << " ms (setup, not measured)\n\n";

  std::vector<std::vector<std::string>> rows;
  rows.reserve(report.rows.size());
  for (const Row& row : report.rows) {
    std::vector<std::string> cells;
    cells.push_back(row.label);
    cells.push_back(std::to_string(row.scale));
    if (row.ran) {
      cells.push_back(std::to_string(row.iterations));
      cells.push_back(format_fixed(row.total_ms, 3));
      cells.push_back(format_fixed(row.us_per_operation, 3));
    } else {
      cells.push_back("0");
      cells.push_back("-");
      cells.push_back("-");
    }
    rows.push_back(std::move(cells));
  }
  std::cout << fo::render_table({"operation", "scale", "iterations", "total ms", "us/op"}, rows)
            << "\n";
}

struct GrowthRow {
  std::string label;
  double cost_ratio = 0.0;
  double exponent = 0.0;
};

void print_observations(const std::vector<ScaleReport>& reports) {
  std::cout << "\n"
            << "observations (computed from the measured microseconds per operation above; "
               "us/op is microseconds per completed operation)\n";

  std::vector<std::size_t> scales;
  for (const ScaleReport& report : reports) {
    bool measured = false;
    for (const Row& row : report.rows) {
      if (row.ran) {
        measured = true;
      }
    }
    if (measured) {
      scales.push_back(report.clusters);
    }
  }
  std::sort(scales.begin(), scales.end());

  std::vector<GrowthRow> growth;
  if (scales.size() < 2) {
    std::cout << "  only one federation size was measured; per-operation growth between sizes is "
                 "not reported\n";
  } else {
    const std::size_t small = scales[scales.size() - 2];
    const std::size_t big = scales.back();
    const double federation_ratio = static_cast<double>(big) / static_cast<double>(small);
    std::cout << "  per-operation cost growth from " << small << " to " << big << " clusters "
              << "(federation x" << format_fixed(federation_ratio, 1) << ")\n\n";

    std::vector<std::vector<std::string>> rows;
    for (const OperationInfo& info : kOperations) {
      const Row* small_row = nullptr;
      const Row* big_row = nullptr;
      for (const ScaleReport& report : reports) {
        for (const Row& row : report.rows) {
          if (row.order != static_cast<std::size_t>(info.op) || !row.ran) {
            continue;
          }
          if (row.scale == small) {
            small_row = &row;
          }
          if (row.scale == big) {
            big_row = &row;
          }
        }
      }
      if (small_row == nullptr || big_row == nullptr || small_row->us_per_operation <= 0.0) {
        continue;
      }
      GrowthRow entry;
      entry.label = big_row->label;
      entry.cost_ratio = big_row->us_per_operation / small_row->us_per_operation;
      entry.exponent = std::log(entry.cost_ratio) / std::log(federation_ratio);
      growth.push_back(entry);

      std::vector<std::string> cells;
      cells.push_back(entry.label);
      cells.push_back(format_fixed(entry.cost_ratio, 2) + "x");
      cells.push_back(format_fixed(entry.exponent, 2));
      rows.push_back(std::move(cells));
    }
    std::cout << fo::render_table({"operation", "cost ratio", "implied exponent"}, rows, "    ")
              << "\n";
    std::cout << "    implied exponent = log(cost ratio) / log(federation ratio): 1.0 means the "
                 "cost grew in proportion to the\n    cluster count, 0.0 means it did not grow "
                 "with the federation at all\n";
  }

  std::cout << "\n  super-linear at the largest measured scale (implied exponent > 1.15)\n";
  bool any_super = false;
  for (const GrowthRow& entry : growth) {
    if (entry.exponent > 1.15) {
      std::cout << "    - " << entry.label << ": " << format_fixed(entry.cost_ratio, 2)
                << "x cost for the federation growth above\n";
      any_super = true;
    }
  }
  if (!any_super) {
    std::cout << "    - none measured\n";
  }

  std::cout << "\n  flat with federation size (implied exponent < 0.15)\n";
  bool any_flat = false;
  for (const GrowthRow& entry : growth) {
    if (entry.exponent < 0.15) {
      std::cout << "    - " << entry.label << ": " << format_fixed(entry.cost_ratio, 2)
                << "x cost for the federation growth above\n";
      any_flat = true;
    }
  }
  if (!any_flat) {
    std::cout << "    - none measured\n";
  }

  std::cout << "\n  notes\n";
  bool any_note = false;
  for (const ScaleReport& report : reports) {
    for (const Row& row : report.rows) {
      if (row.note.empty()) {
        continue;
      }
      std::cout << "    - scale " << row.scale << ": " << row.label << " was not measured: "
                << row.note << "; the cell reads \"-\" rather than a fabricated number.\n";
      any_note = true;
    }
  }
  for (const ScaleReport& report : reports) {
    for (const Row& row : report.rows) {
      if (!row.ran || row.order != order_of(Operation::RegisterCluster)) {
        continue;
      }
      std::cout << "    - cluster publication is measured on a topology-only fixture of "
                << report.clusters
                << " clusters by registering further clusters into it\n      (bounded to at most a "
                   "25% increase and at least 16 registrations), so its per-operation cost\n"
                   "      reflects a federation slightly larger than "
                << report.clusters << " clusters.\n";
      any_note = true;
    }
  }
  for (const ScaleReport& report : reports) {
    if (report.clusters > 1) {
      continue;
    }
    for (const Row& row : report.rows) {
      if (!row.ran || row.order != order_of(Operation::PublishPlacement)) {
        continue;
      }
      std::cout << "    - scale 1: the placement record carries one candidate, because a "
                   "candidate must name a registered\n      cluster and a four-candidate record "
                   "cannot be published into a one-cluster federation. The row label states\n"
                   "      the candidate count actually published at each scale.\n";
      any_note = true;
    }
  }
  if (!any_note) {
    std::cout << "    - none\n";
  }
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
  bool help = false;
  bool has_scale = false;
  std::size_t scale = 0;
  Selection selection;
};

void print_usage(std::ostream& out) {
  out << "usage: fo-benchmarks [--scale <1|10|100|1000|10000>] [--only <substring>]... [--help]\n"
         "\n"
         "  --scale <n>        run only the federation size n (default: every planned scale)\n"
         "  --only <text>      run only operations whose key or name contains <text>\n"
         "                     (case-insensitive; may be repeated, matches are unioned)\n"
         "  --help             print this message\n";
}

bool parse_unsigned(const std::string& text, std::size_t& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  out = value;
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      return true;
    }
    if (argument == "--scale") {
      if (index + 1 >= argc) {
        std::cerr << "--scale requires a value\n";
        return false;
      }
      std::size_t value = 0;
      if (!parse_unsigned(argv[++index], value)) {
        std::cerr << "--scale requires a positive integer\n";
        return false;
      }
      bool known = false;
      for (const ScalePlan& plan : kPlans) {
        if (plan.clusters == value) {
          known = true;
        }
      }
      if (!known) {
        std::cerr << "--scale " << value
                  << " is not a planned scale (1, 10, 100, 1000, 10000)\n";
        return false;
      }
      options.has_scale = true;
      options.scale = value;
      continue;
    }
    if (argument == "--only") {
      if (index + 1 >= argc) {
        std::cerr << "--only requires a value\n";
        return false;
      }
      options.selection.add(argv[++index]);
      continue;
    }
    std::cerr << "unrecognized argument: " << argument << "\n";
    return false;
  }
  return true;
}

void print_banner() {
  std::cout << "Federation Observatory " << fo::version_string()
            << " benchmarks - completed operations\n"
               "The runtime under measurement is observational: it does not choose placements, "
               "does not migrate\n"
               "workloads and does not own policy. Every record published here is synthetic "
               "evidence built by the\n"
               "harness and attributed to publisher \"bench-publisher\"; the scheduling decisions "
               "it describes belong\n"
               "to the upstream scheduler and are only observed. Fixture setup is excluded from "
               "every measurement,\n"
               "iterations exclude the single warm-up run of each operation, and us/op is "
               "microseconds per completed\n"
               "operation measured with std::chrono::steady_clock.\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage(std::cerr);
    return 2;
  }
  if (options.help) {
    print_usage(std::cout);
    return 0;
  }

  print_banner();

  std::vector<ScaleReport> reports;
  std::size_t measured_rows = 0;
  try {
    for (const ScalePlan& plan : kPlans) {
      if (options.has_scale && plan.clusters != options.scale) {
        continue;
      }
      ScaleReport report = run_scale(plan, options.selection);
      if (report.rows.empty()) {
        continue;
      }
      measured_rows += report.rows.size();
      print_scale_report(report);
      reports.push_back(std::move(report));
    }
  } catch (const BenchError& error) {
    std::cerr << "benchmark aborted: " << error.detail << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "benchmark aborted with an unexpected exception: " << error.what() << "\n";
    return 1;
  }

  if (measured_rows == 0) {
    std::cerr << "no operation matched the requested --only filter\n";
    return 2;
  }

  print_observations(reports);
  return 0;
}
