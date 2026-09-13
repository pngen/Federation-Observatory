// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Federation, site, cluster, accelerator, runtime, backend, domain, artifact, workload
// and policy records: semantic validation and deterministic rendering.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/model.hpp"

namespace fo {
namespace {

template <class T>
bool has_duplicates(std::vector<T> values) {
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

std::string generations_line(const ObservationStamp& stamp) {
  return std::string("publisher=") + render_id(stamp.publisher.value()) +
         " boot=" + (stamp.publisher_boot.is_set() ? stamp.publisher_boot.to_string() : "-") +
         " epoch=" + (stamp.coordinator_epoch.is_set() ? stamp.coordinator_epoch.to_string() : "-") +
         " evidence_gen=" +
         (stamp.evidence_generation.is_set() ? stamp.evidence_generation.to_string() : "-") +
         " seq=" + stamp.sequence.to_string();
}

std::string gen_or_dash(std::uint64_t value) { return value == 0 ? std::string("-") : std::to_string(value); }

}  // namespace

std::string_view to_string(Currentness currentness) noexcept {
  switch (currentness) {
    case Currentness::Unknown: return "UNKNOWN";
    case Currentness::Current: return "CURRENT";
    case Currentness::Stale: return "STALE";
    case Currentness::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case Currentness::Retired: return "RETIRED";
  }
  return "UNKNOWN";
}

bool parse_currentness(std::string_view text, Currentness& out) noexcept {
  for (int i = 0; i <= static_cast<int>(Currentness::Retired); ++i) {
    const auto candidate = static_cast<Currentness>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool is_current(Currentness currentness) noexcept { return currentness == Currentness::Current; }

std::string_view to_string(Readiness readiness) noexcept {
  switch (readiness) {
    case Readiness::Unknown: return "UNKNOWN";
    case Readiness::Ready: return "READY";
    case Readiness::NotReady: return "NOT_READY";
    case Readiness::Draining: return "DRAINING";
    case Readiness::Retired: return "RETIRED";
  }
  return "UNKNOWN";
}

bool parse_readiness(std::string_view text, Readiness& out) noexcept {
  for (int i = 0; i <= static_cast<int>(Readiness::Retired); ++i) {
    const auto candidate = static_cast<Readiness>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(RuntimeKind kind) noexcept {
  switch (kind) {
    case RuntimeKind::Unknown: return "UNKNOWN";
    case RuntimeKind::Cuda: return "CUDA";
    case RuntimeKind::Rocm: return "ROCM";
    case RuntimeKind::OneApi: return "ONEAPI";
    case RuntimeKind::Metal: return "METAL";
    case RuntimeKind::Vulkan: return "VULKAN";
    case RuntimeKind::OpenCl: return "OPENCL";
    case RuntimeKind::CpuGeneric: return "CPU_GENERIC";
    case RuntimeKind::Custom: return "CUSTOM";
  }
  return "UNKNOWN";
}

bool parse_runtime_kind(std::string_view text, RuntimeKind& out) noexcept {
  for (int i = 0; i <= static_cast<int>(RuntimeKind::Custom); ++i) {
    const auto candidate = static_cast<RuntimeKind>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ArtifactKind kind) noexcept {
  switch (kind) {
    case ArtifactKind::Unknown: return "UNKNOWN";
    case ArtifactKind::ExecutableBinary: return "EXECUTABLE_BINARY";
    case ArtifactKind::SharedLibrary: return "SHARED_LIBRARY";
    case ArtifactKind::DeviceImage: return "DEVICE_IMAGE";
    case ArtifactKind::ContainerImage: return "CONTAINER_IMAGE";
    case ArtifactKind::ModelWeights: return "MODEL_WEIGHTS";
    case ArtifactKind::Adapter: return "ADAPTER";
    case ArtifactKind::Checkpoint: return "CHECKPOINT";
    case ArtifactKind::DatasetShard: return "DATASET_SHARD";
    case ArtifactKind::Custom: return "CUSTOM";
  }
  return "UNKNOWN";
}

bool parse_artifact_kind(std::string_view text, ArtifactKind& out) noexcept {
  for (int i = 0; i <= static_cast<int>(ArtifactKind::Custom); ++i) {
    const auto candidate = static_cast<ArtifactKind>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(DomainKind kind) noexcept {
  switch (kind) {
    case DomainKind::Unknown: return "UNKNOWN";
    case DomainKind::Cluster: return "CLUSTER";
    case DomainKind::Site: return "SITE";
    case DomainKind::Federation: return "FEDERATION";
  }
  return "UNKNOWN";
}

bool parse_domain_kind(std::string_view text, DomainKind& out) noexcept {
  for (int i = 0; i <= static_cast<int>(DomainKind::Federation); ++i) {
    const auto candidate = static_cast<DomainKind>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool operator==(const ObservationStamp& a, const ObservationStamp& b) noexcept {
  return a.publisher == b.publisher && a.publisher_boot == b.publisher_boot &&
         a.coordinator_epoch == b.coordinator_epoch &&
         a.evidence_generation == b.evidence_generation && a.sequence == b.sequence &&
         a.observed_at == b.observed_at && a.precision == b.precision &&
         a.evidence_class == b.evidence_class && a.provenance == b.provenance;
}

std::string ObservationStamp::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"publisher", render_id(publisher.value())});
  rows.push_back({"publisher_boot", publisher_boot.is_set() ? publisher_boot.to_string() : "-"});
  rows.push_back({"coordinator_epoch", coordinator_epoch.is_set() ? coordinator_epoch.to_string() : "-"});
  rows.push_back({"evidence_generation", evidence_generation.is_set() ? evidence_generation.to_string() : "-"});
  rows.push_back({"sequence", sequence.to_string()});
  rows.push_back({"observed_at", format_timestamp(observed_at)});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"provenance", std::string(fo::to_string(provenance))});
  return render_table({"stamp", "value"}, rows, indent);
}

Status FederationRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "federation id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "federation generation is unset", id.value());
  }
  if (!coordinator_epoch.is_set()) {
    return fail(ErrorCode::InvalidArgument, "coordinator epoch is unset", id.value());
  }
  if (has_duplicates(sites)) {
    return fail(ErrorCode::InvalidArgument, "duplicate site reference", id.value());
  }
  if (has_duplicates(clusters)) {
    return fail(ErrorCode::InvalidArgument, "duplicate cluster reference", id.value());
  }
  if (has_duplicates(accelerator_classes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate accelerator class reference", id.value());
  }
  if (has_duplicates(runtimes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate runtime reference", id.value());
  }
  if (has_duplicates(workload_classes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate workload class reference", id.value());
  }
  return Status::success();
}

bool FederationRecord::contains_cluster(const ClusterId& cluster) const {
  return std::find(clusters.begin(), clusters.end(), cluster) != clusters.end();
}

bool FederationRecord::contains_site(const SiteId& site) const {
  return std::find(sites.begin(), sites.end(), site) != sites.end();
}

std::string FederationRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"display_name", display_name.empty() ? "-" : display_name});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"coordinator_epoch", coordinator_epoch.to_string()});
  rows.push_back({"sites", std::to_string(sites.size())});
  rows.push_back({"clusters", std::to_string(clusters.size())});
  rows.push_back({"accelerator_classes", std::to_string(accelerator_classes.size())});
  rows.push_back({"runtimes", std::to_string(runtimes.size())});
  rows.push_back({"workload_classes", std::to_string(workload_classes.size())});
  rows.push_back({"policy", render_id(policy.value())});
  rows.push_back({"policy_generation", policy_generation.is_set() ? policy_generation.to_string() : "-"});
  rows.push_back({"compatibility_generation",
                  compatibility_generation.is_set() ? compatibility_generation.to_string() : "-"});
  rows.push_back({"capacity_generation",
                  capacity_generation.is_set() ? capacity_generation.to_string() : "-"});
  rows.push_back({"topology_generation",
                  topology_generation.is_set() ? topology_generation.to_string() : "-"});
  rows.push_back({"evidence_generation",
                  evidence_generation.is_set() ? evidence_generation.to_string() : "-"});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"evidence_class", std::string(fo::to_string(stamp.evidence_class))});
  std::string out = std::string(indent) + "federation:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += indent_block(evidence.render(std::string(indent) + "  "), "");
  return out;
}

Status SiteRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "site id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "site generation is unset", id.value());
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "site has no federation", id.value());
  }
  if (has_duplicates(clusters)) {
    return fail(ErrorCode::InvalidArgument, "duplicate cluster reference", id.value());
  }
  if (clusters.size() > kMaxClusterPerSite) {
    return fail(ErrorCode::BoundExceeded, "too many clusters in site", id.value());
  }
  if (has_duplicates(policies)) {
    return fail(ErrorCode::InvalidArgument, "duplicate policy reference", id.value());
  }
  return Status::success();
}

std::string SiteRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"region", region.empty() ? "-" : region});
  rows.push_back({"zone", zone.empty() ? "-" : zone});
  rows.push_back({"failure_domain", failure_domain.empty() ? "-" : failure_domain});
  rows.push_back({"clusters", std::to_string(clusters.size())});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "site:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status AcceleratorClassRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "accelerator class id is empty");
  }
  if (!capability_generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "accelerator capability generation is unset", id.value());
  }
  for (const CapabilityEntry& entry : capabilities.entries()) {
    if (!entry.key.valid()) {
      return fail(ErrorCode::InvalidArgument, "invalid capability key on accelerator class",
                  id.value() + " " + entry.key.to_string());
    }
  }
  return Status::success();
}

std::string AcceleratorClassRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"capability_generation", capability_generation.to_string()});
  rows.push_back({"vendor", vendor.empty() ? "UNKNOWN" : vendor});
  rows.push_back({"family", family.empty() ? "UNKNOWN" : family});
  rows.push_back({"model", model.empty() ? "UNKNOWN" : model});
  rows.push_back({"architecture", architecture.empty() ? "UNKNOWN" : architecture});
  rows.push_back({"compute_capability", compute_capability.empty() ? "UNKNOWN" : compute_capability});
  rows.push_back({"memory_bytes_per_device",
                  memory_bytes_per_device == 0 ? "UNKNOWN" : std::to_string(memory_bytes_per_device)});
  rows.push_back({"memory_bandwidth_gbps",
                  memory_bandwidth_gbps == 0 ? "UNKNOWN" : std::to_string(memory_bandwidth_gbps)});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  std::string out = std::string(indent) + "accelerator_class:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += indent_block(capabilities.render(std::string(indent) + "  "), "");
  return out;
}

Status RuntimeRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "runtime id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "runtime generation is unset", id.value());
  }
  return Status::success();
}

std::string RuntimeRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"backend", render_id(backend.value())});
  rows.push_back({"kind", std::string(fo::to_string(kind))});
  rows.push_back({"version", version.empty() ? "UNKNOWN" : version});
  rows.push_back({"abi", abi.empty() ? "UNKNOWN" : abi});
  rows.push_back({"driver_version", driver_version.empty() ? "UNKNOWN" : driver_version});
  rows.push_back({"driver_abi", driver_abi.empty() ? "UNKNOWN" : driver_abi});
  rows.push_back({"compiler_version", compiler_version.empty() ? "UNKNOWN" : compiler_version});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  std::string out = std::string(indent) + "runtime:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += indent_block(capabilities.render(std::string(indent) + "  "), "");
  return out;
}

Status BackendRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "backend id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "backend generation is unset", id.value());
  }
  if (runtime.empty()) {
    return fail(ErrorCode::InvalidArgument, "backend has no runtime", id.value());
  }
  return Status::success();
}

std::string BackendRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"runtime", runtime.value()});
  rows.push_back({"name", name.empty() ? "-" : name});
  rows.push_back({"version", version.empty() ? "UNKNOWN" : version});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "backend:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status ClusterRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "cluster id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "cluster generation is unset", id.value());
  }
  if (!epoch.is_set()) {
    return fail(ErrorCode::InvalidArgument, "cluster epoch is unset", id.value());
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "cluster has no federation", id.value());
  }
  if (site.empty()) {
    return fail(ErrorCode::InvalidArgument, "cluster has no site", id.value());
  }
  if (has_duplicates(accelerator_classes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate accelerator class reference", id.value());
  }
  if (has_duplicates(runtimes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate runtime reference", id.value());
  }
  if (has_duplicates(backends)) {
    return fail(ErrorCode::InvalidArgument, "duplicate backend reference", id.value());
  }
  if (capacity_pools.size() > bounds::kMaxPoolsPerCluster) {
    return fail(ErrorCode::BoundExceeded, "too many capacity pools on cluster", id.value());
  }
  std::vector<ResourcePoolId> pool_ids;
  pool_ids.reserve(capacity_pools.size());
  for (const CapacityPool& pool : capacity_pools) {
    if (pool.pool_id.empty()) {
      return fail(ErrorCode::InvalidArgument, "capacity pool has no id", id.value());
    }
    const Status ledger_status = pool.ledger.validate();
    if (!ledger_status.ok()) {
      return fail(ErrorCode::CapacityInconsistent, "capacity pool ledger does not close",
                  id.value() + " " + pool.pool_id.value() + ": " + ledger_status.error().message());
    }
    if (pool.kind == ResourceKind::Accelerator && pool.accelerator_class.empty()) {
      return fail(ErrorCode::InvalidArgument, "accelerator pool has no accelerator class",
                  id.value() + " " + pool.pool_id.value());
    }
    if (pool.kind == ResourceKind::Custom && pool.custom_name.empty()) {
      return fail(ErrorCode::InvalidArgument, "custom pool has no resource name",
                  id.value() + " " + pool.pool_id.value());
    }
    pool_ids.push_back(pool.pool_id);
  }
  if (has_duplicates(pool_ids)) {
    return fail(ErrorCode::InvalidArgument, "duplicate capacity pool id", id.value());
  }
  for (const CapabilityEntry& entry : capabilities.entries()) {
    if (!entry.key.valid()) {
      return fail(ErrorCode::InvalidArgument, "invalid capability key on cluster",
                  id.value() + " " + entry.key.to_string());
    }
  }
  return Status::success();
}

const CapacityPool* ClusterRecord::find_pool(const ResourcePoolId& pool) const {
  for (const CapacityPool& candidate : capacity_pools) {
    if (candidate.pool_id == pool) {
      return &candidate;
    }
  }
  return nullptr;
}

Result<CapacityLedger> ClusterRecord::total_ledger(ResourceKind kind) const {
  CapacityLedger total;
  for (const CapacityPool& pool : capacity_pools) {
    if (pool.kind != kind) {
      continue;
    }
    std::uint64_t nominal = 0;
    std::uint64_t offline = 0;
    std::uint64_t allocated = 0;
    std::uint64_t reserved = 0;
    std::uint64_t draining = 0;
    std::uint64_t unusable = 0;
    if (!checked_add(total.nominal, pool.ledger.nominal, nominal) ||
        !checked_add(total.offline, pool.ledger.offline, offline) ||
        !checked_add(total.allocated, pool.ledger.allocated, allocated) ||
        !checked_add(total.reserved, pool.ledger.reserved, reserved) ||
        !checked_add(total.draining, pool.ledger.draining, draining) ||
        !checked_add(total.unusable, pool.ledger.unusable, unusable)) {
      return Error(ErrorCode::CapacityInconsistent, "capacity total overflowed",
                   id.value() + " " + std::string(fo::to_string(kind)));
    }
    const Result<CapacityLedger> rebuilt =
        CapacityLedger::from_components(nominal, offline, allocated, reserved, draining, unusable);
    if (!rebuilt.ok()) {
      return rebuilt.error();
    }
    total = rebuilt.value();
  }
  return total;
}

std::string ClusterRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"site", site.value()});
  rows.push_back({"domain", render_id(domain.value())});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"epoch", epoch.to_string()});
  rows.push_back({"failure_domain", failure_domain.empty() ? "-" : failure_domain});
  rows.push_back({"zone", zone.empty() ? "-" : zone});
  rows.push_back({"readiness", std::string(fo::to_string(readiness))});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"accelerator_classes", std::to_string(accelerator_classes.size())});
  rows.push_back({"runtimes", std::to_string(runtimes.size())});
  rows.push_back({"topology_generation", gen_or_dash(topology_generation.value())});
  rows.push_back({"capability_generation", gen_or_dash(capability_generation.value())});
  rows.push_back({"capacity_generation", gen_or_dash(capacity_generation.value())});
  rows.push_back({"runtime_generation", gen_or_dash(runtime_generation.value())});
  rows.push_back({"compatibility_generation", gen_or_dash(compatibility_generation.value())});
  rows.push_back({"evidence_generation", gen_or_dash(evidence_generation.value())});
  rows.push_back({"stamp", generations_line(stamp)});
  std::string out = std::string(indent) + "cluster:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!capacity_pools.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> pool_rows;
    for (const CapacityPool& pool : capacity_pools) {
      pool_rows.push_back({pool.pool_id.value(), std::string(fo::to_string(pool.kind)),
                           render_id(pool.accelerator_class.value()),
                           std::to_string(pool.ledger.nominal), std::to_string(pool.ledger.offline),
                           std::to_string(pool.ledger.allocated), std::to_string(pool.ledger.reserved),
                           std::to_string(pool.ledger.draining), std::to_string(pool.ledger.unusable),
                           std::to_string(pool.ledger.idle)});
    }
    out += render_table({"pool", "kind", "class", "nominal", "offline", "allocated", "reserved",
                         "draining", "unusable", "idle"},
                        pool_rows, std::string(indent) + "  ");
    out += '\n';
  }
  out += indent_block(capabilities.render(std::string(indent) + "  "), "");
  return out;
}

Status DomainRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "domain id is empty");
  }
  if (kind == DomainKind::Unknown) {
    return fail(ErrorCode::InvalidArgument, "domain kind is unknown", id.value());
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "domain has no federation", id.value());
  }
  if (kind == DomainKind::Site && site.empty()) {
    return fail(ErrorCode::InvalidArgument, "site domain has no site", id.value());
  }
  if (clusters.empty()) {
    return fail(ErrorCode::InvalidArgument, "domain has no member clusters", id.value());
  }
  if (has_duplicates(clusters)) {
    return fail(ErrorCode::InvalidArgument, "duplicate cluster reference", id.value());
  }
  return Status::success();
}

std::string DomainRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"kind", std::string(fo::to_string(kind))});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"site", render_id(site.value())});
  rows.push_back({"clusters", std::to_string(clusters.size())});
  rows.push_back({"topology_generation", gen_or_dash(topology_generation.value())});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "domain:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status ArtifactRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "artifact id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "artifact generation is unset", id.value());
  }
  if (has_duplicates(target_architectures)) {
    return fail(ErrorCode::InvalidArgument, "duplicate target architecture", id.value());
  }
  for (const std::string& architecture : target_architectures) {
    if (architecture.empty() || architecture.size() > 64) {
      return fail(ErrorCode::InvalidArgument, "target architecture is malformed", id.value());
    }
  }
  return Status::success();
}

bool ArtifactRecord::targets_architecture(std::string_view architecture) const {
  if (target_architectures.empty()) {
    return false;
  }
  for (const std::string& candidate : target_architectures) {
    if (candidate == architecture) {
      return true;
    }
  }
  return false;
}

std::string ArtifactRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"kind", std::string(fo::to_string(kind))});
  rows.push_back({"format", format.empty() ? "UNKNOWN" : format});
  rows.push_back({"kernel_format", kernel_format.empty() ? "UNKNOWN" : kernel_format});
  rows.push_back({"target_architectures",
                  target_architectures.empty() ? "UNKNOWN" : join_strings(target_architectures, ",")});
  rows.push_back({"minimum_compute_capability",
                  minimum_compute_capability.empty() ? "UNKNOWN" : minimum_compute_capability});
  rows.push_back({"size_bytes", std::to_string(size_bytes)});
  rows.push_back({"runtime_abi", runtime_abi.empty() ? "UNKNOWN" : runtime_abi});
  rows.push_back({"driver_abi", driver_abi.empty() ? "UNKNOWN" : driver_abi});
  rows.push_back({"compiler_target", compiler_target.empty() ? "UNKNOWN" : compiler_target});
  rows.push_back({"state_format", state_format.empty() ? "UNKNOWN" : state_format});
  rows.push_back({"required_memory_bytes", std::to_string(required_memory_bytes)});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "artifact:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status WorkloadClassRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "workload class id is empty");
  }
  return Status::success();
}

std::string WorkloadClassRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"display_name", display_name.empty() ? "-" : display_name});
  rows.push_back({"prefers_accelerators", prefers_accelerators ? "true" : "false"});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "workload_class:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

Status WorkloadRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "workload id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "workload generation is unset", id.value());
  }
  if (workload_class.empty()) {
    return fail(ErrorCode::InvalidArgument, "workload has no class", id.value());
  }
  if (required_accelerators == 0) {
    return fail(ErrorCode::InvalidArgument, "workload requires zero accelerators", id.value());
  }
  if (has_duplicates(acceptable_accelerator_classes)) {
    return fail(ErrorCode::InvalidArgument, "duplicate acceptable accelerator class", id.value());
  }
  for (const CapabilityRequirement& requirement : requirements) {
    if (!requirement.key.valid()) {
      return fail(ErrorCode::InvalidArgument, "requirement has an invalid capability key",
                  id.value() + " " + requirement.key.to_string());
    }
  }
  if (required_domain_kind == DomainKind::Unknown && !required_domain.empty()) {
    return fail(ErrorCode::InvalidArgument, "workload names a domain without a domain kind",
                id.value());
  }
  if (requirements.size() > kMaxCapabilityEntriesPerPublication) {
    return fail(ErrorCode::BoundExceeded, "too many capability requirements", id.value());
  }
  return Status::success();
}

std::string WorkloadRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"class", workload_class.value()});
  rows.push_back({"artifact", render_id(artifact.value())});
  rows.push_back({"artifact_generation", gen_or_dash(artifact_generation.value())});
  rows.push_back({"required_accelerators", std::to_string(required_accelerators)});
  rows.push_back({"required_memory_bytes_per_accelerator",
                  std::to_string(required_memory_bytes_per_accelerator)});
  rows.push_back({"acceptable_accelerator_classes",
                  acceptable_accelerator_classes.empty()
                      ? "ANY"
                      : join_strings([&] {
                          std::vector<std::string> names;
                          for (const AcceleratorClassId& c : acceptable_accelerator_classes) {
                            names.push_back(c.value());
                          }
                          return names;
                        }(),
                                     ",")});
  rows.push_back({"required_domain", render_id(required_domain.value())});
  rows.push_back({"required_domain_kind", std::string(fo::to_string(required_domain_kind))});
  rows.push_back({"require_single_failure_domain",
                  require_single_failure_domain ? "true" : "false"});
  rows.push_back({"policy", render_id(policy.value())});
  rows.push_back({"isolation_requirement",
                  isolation_requirement.empty() ? "-" : isolation_requirement});
  rows.push_back({"portability_class", portability_class.empty() ? "-" : portability_class});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  std::string out = std::string(indent) + "workload:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!requirements.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> req_rows;
    for (const CapabilityRequirement& requirement : requirements) {
      req_rows.push_back({requirement.key.to_string(),
                          std::string(fo::to_string(requirement.comparator)),
                          requirement.value.known() ? requirement.value.to_string() : "-",
                          requirement.optional ? "optional" : "required",
                          requirement.rationale});
    }
    out += render_table({"capability", "comparator", "value", "necessity", "rationale"}, req_rows,
                        std::string(indent) + "  ");
    out += '\n';
  }
  return out;
}

Status PolicyRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "policy id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "policy generation is unset", id.value());
  }
  if (has_duplicates(allowed_sites) || has_duplicates(denied_sites)) {
    return fail(ErrorCode::InvalidArgument, "duplicate site reference in policy", id.value());
  }
  if (has_duplicates(allowed_clusters) || has_duplicates(denied_clusters)) {
    return fail(ErrorCode::InvalidArgument, "duplicate cluster reference in policy", id.value());
  }
  for (const SiteId& site : allowed_sites) {
    if (std::find(denied_sites.begin(), denied_sites.end(), site) != denied_sites.end()) {
      return fail(ErrorCode::InvalidArgument, "site is both allowed and denied", id.value());
    }
  }
  for (const ClusterId& cluster : allowed_clusters) {
    if (std::find(denied_clusters.begin(), denied_clusters.end(), cluster) != denied_clusters.end()) {
      return fail(ErrorCode::InvalidArgument, "cluster is both allowed and denied", id.value());
    }
  }
  return Status::success();
}

Tri PolicyRecord::permits_site(const SiteId& site) const {
  if (std::find(denied_sites.begin(), denied_sites.end(), site) != denied_sites.end()) {
    return Tri::No;
  }
  if (allowed_sites.empty()) {
    return Tri::Yes;
  }
  return std::find(allowed_sites.begin(), allowed_sites.end(), site) != allowed_sites.end() ? Tri::Yes
                                                                                            : Tri::No;
}

Tri PolicyRecord::permits_cluster(const ClusterId& cluster) const {
  if (std::find(denied_clusters.begin(), denied_clusters.end(), cluster) != denied_clusters.end()) {
    return Tri::No;
  }
  if (allowed_clusters.empty()) {
    return Tri::Yes;
  }
  return std::find(allowed_clusters.begin(), allowed_clusters.end(), cluster) != allowed_clusters.end()
             ? Tri::Yes
             : Tri::No;
}

std::string PolicyRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"name", name.empty() ? "-" : name});
  rows.push_back({"allowed_sites", std::to_string(allowed_sites.size())});
  rows.push_back({"denied_sites", std::to_string(denied_sites.size())});
  rows.push_back({"allowed_clusters", std::to_string(allowed_clusters.size())});
  rows.push_back({"denied_clusters", std::to_string(denied_clusters.size())});
  rows.push_back({"allow_cross_site", std::string(fo::to_string(allow_cross_site))});
  rows.push_back({"data_residency", data_residency.empty() ? "-" : data_residency});
  rows.push_back({"isolation_requirement",
                  isolation_requirement.empty() ? "-" : isolation_requirement});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  return std::string(indent) + "policy:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

}  // namespace fo
