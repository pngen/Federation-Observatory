// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Immutable snapshot accessors, snapshot rendering and digests, and the enum vocabulary
// of the analysis layer. A snapshot is a value: every accessor returns a pointer into
// the immutable object the caller already holds, and nothing here mutates anything.

#include <algorithm>
#include <string>
#include <vector>

#include "federation_observatory/observatory.hpp"

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/snapshot.hpp"

namespace fo {

namespace {

/// Snapshot collections are maintained in identifier order (see build_snapshot), so a
/// lookup is a binary search rather than a scan. A linear scan here turns every analysis
/// that resolves a cluster per finding into an O(N^2) pass, which the benchmarks showed
/// directly: fragmentation over 10,000 clusters cost 286 ms per call before this change.
template <class Record, class Id>
const Record* find_by_id(const std::vector<Record>& records, const Id& id) {
  const auto it = std::lower_bound(
      records.begin(), records.end(), id,
      [](const Record& record, const Id& key) { return record.id < key; });
  if (it == records.end() || !(it->id == id)) {
    return nullptr;
  }
  return &*it;
}

}  // namespace

std::string_view to_string(FragmentationClass cls) noexcept {
  switch (cls) {
    case FragmentationClass::None: return "NONE";
    case FragmentationClass::CapacityShortage: return "CAPACITY_SHORTAGE";
    case FragmentationClass::PhysicalFragmentation: return "PHYSICAL_FRAGMENTATION";
    case FragmentationClass::CompatibilityFragmentation: return "COMPATIBILITY_FRAGMENTATION";
    case FragmentationClass::PolicyFragmentation: return "POLICY_FRAGMENTATION";
    case FragmentationClass::TopologyFragmentation: return "TOPOLOGY_FRAGMENTATION";
    case FragmentationClass::SiteFragmentation: return "SITE_FRAGMENTATION";
    case FragmentationClass::Mixed: return "MIXED";
    case FragmentationClass::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool parse_fragmentation_class(std::string_view text, FragmentationClass& out) noexcept {
  for (int i = 0; i <= static_cast<int>(FragmentationClass::Unknown); ++i) {
    const auto candidate = static_cast<FragmentationClass>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(DriftKind kind) noexcept {
  switch (kind) {
    case DriftKind::ClusterMissing: return "CLUSTER_MISSING";
    case DriftKind::UnexpectedCluster: return "UNEXPECTED_CLUSTER";
    case DriftKind::SiteMissing: return "SITE_MISSING";
    case DriftKind::UnexpectedSite: return "UNEXPECTED_SITE";
    case DriftKind::CapabilityMismatch: return "CAPABILITY_MISMATCH";
    case DriftKind::RuntimeVersionDivergence: return "RUNTIME_VERSION_DIVERGENCE";
    case DriftKind::StaleCluster: return "STALE_CLUSTER";
    case DriftKind::PolicyGenerationMismatch: return "POLICY_GENERATION_MISMATCH";
    case DriftKind::ArtifactCompatibilityDrift: return "ARTIFACT_COMPATIBILITY_DRIFT";
    case DriftKind::CapacityReportDivergence: return "CAPACITY_REPORT_DIVERGENCE";
    case DriftKind::PlacementTargetChanged: return "PLACEMENT_TARGET_CHANGED";
    case DriftKind::PlacementRejectionIntroduced: return "PLACEMENT_REJECTION_INTRODUCED";
    case DriftKind::PlacementRejectionCleared: return "PLACEMENT_REJECTION_CLEARED";
    case DriftKind::PlacementFallbackIntroduced: return "PLACEMENT_FALLBACK_INTRODUCED";
    case DriftKind::PlacementCompatibilityChanged: return "PLACEMENT_COMPATIBILITY_CHANGED";
  }
  return "CLUSTER_MISSING";
}

bool parse_drift_kind(std::string_view text, DriftKind& out) noexcept {
  for (int i = 0; i <= static_cast<int>(DriftKind::PlacementCompatibilityChanged); ++i) {
    const auto candidate = static_cast<DriftKind>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool is_behavioral_drift(DriftKind kind) noexcept {
  switch (kind) {
    case DriftKind::PlacementTargetChanged:
    case DriftKind::PlacementRejectionIntroduced:
    case DriftKind::PlacementRejectionCleared:
    case DriftKind::PlacementFallbackIntroduced:
    case DriftKind::PlacementCompatibilityChanged:
      return true;
    default:
      return false;
  }
}

bool requires_intended_state(DriftKind kind) noexcept { return !is_behavioral_drift(kind); }

std::string TimeWindow::render() const {
  if (unbounded()) {
    return "[unbounded]";
  }
  return "[" + (from == 0 ? std::string("-inf") : format_timestamp(from)) + ", " +
         (to == 0 ? std::string("+inf") : format_timestamp(to)) + ")";
}

Status IntendedFederationState::validate() const {
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "intended federation state has no federation");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "intended federation state has no generation",
                federation.value());
  }
  std::vector<SiteId> sites = expected_sites;
  std::sort(sites.begin(), sites.end());
  if (std::adjacent_find(sites.begin(), sites.end()) != sites.end()) {
    return fail(ErrorCode::InvalidArgument, "intended state repeats a site", federation.value());
  }
  std::vector<ClusterId> clusters = expected_clusters;
  std::sort(clusters.begin(), clusters.end());
  if (std::adjacent_find(clusters.begin(), clusters.end()) != clusters.end()) {
    return fail(ErrorCode::InvalidArgument, "intended state repeats a cluster", federation.value());
  }
  for (const ExpectedCapability& expected : expected_capabilities) {
    if (expected.cluster.empty()) {
      return fail(ErrorCode::InvalidArgument, "intended capability has no cluster",
                  federation.value());
    }
    if (!expected.key.valid()) {
      return fail(ErrorCode::InvalidArgument, "intended capability key is not valid",
                  expected.key.to_string());
    }
  }
  for (const auto& entry : expected_runtime_versions) {
    if (entry.first.empty()) {
      return fail(ErrorCode::InvalidArgument, "intended runtime version has no cluster",
                  federation.value());
    }
    if (entry.second.empty()) {
      return fail(ErrorCode::InvalidArgument, "intended runtime version is empty",
                  entry.first.value());
    }
  }
  for (const auto& entry : expected_capacity) {
    if (entry.first.empty()) {
      return fail(ErrorCode::InvalidArgument, "intended capacity has no cluster",
                  federation.value());
    }
    const Status ledger = entry.second.validate();
    if (!ledger.ok()) {
      return fail(ErrorCode::CapacityInconsistent,
                  "intended capacity ledger does not satisfy the accounting identity",
                  entry.first.value() + ": " + ledger.error().message());
    }
  }
  if (expected_clusters.size() > 65536 || expected_sites.size() > 65536 ||
      expected_capabilities.size() > 65536) {
    return fail(ErrorCode::BoundExceeded, "intended federation state exceeds a configured bound",
                federation.value());
  }
  return Status::success();
}

const FederationRecord* FederationSnapshot::find_federation(const FederationId& id) const {
  return find_by_id(federations, id);
}

const ClusterRecord* FederationSnapshot::find_cluster(const ClusterId& id) const {
  return find_by_id(clusters, id);
}

const SiteRecord* FederationSnapshot::find_site(const SiteId& id) const {
  return find_by_id(sites, id);
}

const AcceleratorClassRecord* FederationSnapshot::find_accelerator_class(
    const AcceleratorClassId& id) const {
  return find_by_id(accelerator_classes, id);
}

const RuntimeRecord* FederationSnapshot::find_runtime(const RuntimeId& id) const {
  return find_by_id(runtimes, id);
}

const ArtifactRecord* FederationSnapshot::find_artifact(const ArtifactId& id) const {
  return find_by_id(artifacts, id);
}

const WorkloadRecord* FederationSnapshot::find_workload(const WorkloadId& id) const {
  return find_by_id(workloads, id);
}

const WorkloadClassRecord* FederationSnapshot::find_workload_class(
    const WorkloadClassId& id) const {
  return find_by_id(workload_classes, id);
}

const PolicyRecord* FederationSnapshot::find_policy(const PolicyId& id) const {
  return find_by_id(policies, id);
}

const PlacementRecord* FederationSnapshot::find_placement(const PlacementId& id) const {
  return find_by_id(placements, id);
}

const MigrationRecord* FederationSnapshot::find_migration(const MigrationId& id) const {
  return find_by_id(migrations, id);
}

const DomainRecord* FederationSnapshot::find_domain(const DomainId& id) const {
  return find_by_id(domains, id);
}

std::string SnapshotHealth::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"clusters_total", std::to_string(clusters_total)});
  rows.push_back({"clusters_current", std::to_string(clusters_current)});
  rows.push_back({"clusters_stale", std::to_string(clusters_stale)});
  rows.push_back({"clusters_revalidation_required",
                  std::to_string(clusters_revalidation_required)});
  rows.push_back({"clusters_retired", std::to_string(clusters_retired)});
  rows.push_back({"clusters_unknown", std::to_string(clusters_unknown)});
  rows.push_back({"sites_total", std::to_string(sites_total)});
  rows.push_back({"sites_current", std::to_string(sites_current)});
  rows.push_back({"publishers_total", std::to_string(publishers_total)});
  rows.push_back({"publishers_live", std::to_string(publishers_live)});
  rows.push_back({"publishers_fenced", std::to_string(publishers_fenced)});
  rows.push_back({"revalidation_pending", revalidation_pending ? "yes" : "no"});
  rows.push_back({"degraded", degraded ? "yes" : "no"});
  std::string out = render_table({"snapshot_health", "value"}, rows, indent);
  if (degraded) {
    out += '\n';
    out += std::string(indent) +
           "note: this snapshot is NOT a fully current picture of the federation; the counters "
           "above say exactly which parts are current, stale or awaiting revalidation.";
  }
  return out;
}

namespace {

/// The snapshot rendering with the creation timestamp optionally suppressed. digest()
/// suppresses it so that the fingerprint of a state does not change merely because it was
/// observed a moment later.
std::string render_snapshot(const FederationSnapshot& snapshot, bool include_created_at,
                            std::string_view indent) {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", render_id(snapshot.federation.value())});
  rows.push_back(
      {"federation_generation", snapshot.generation.is_set() ? snapshot.generation.to_string() : "-"});
  rows.push_back({"coordinator_epoch", snapshot.coordinator_epoch.is_set()
                                           ? snapshot.coordinator_epoch.to_string()
                                           : "-"});
  rows.push_back({"snapshot_generation", snapshot.snapshot_generation.is_set()
                                             ? snapshot.snapshot_generation.to_string()
                                             : "-"});
  rows.push_back({"created_at",
                  include_created_at ? format_timestamp(snapshot.created_at) : std::string("-")});
  rows.push_back({"federations", std::to_string(snapshot.federations.size())});
  rows.push_back({"sites", std::to_string(snapshot.sites.size())});
  rows.push_back({"clusters", std::to_string(snapshot.clusters.size())});
  rows.push_back({"accelerator_classes", std::to_string(snapshot.accelerator_classes.size())});
  rows.push_back({"runtimes", std::to_string(snapshot.runtimes.size())});
  rows.push_back({"backends", std::to_string(snapshot.backends.size())});
  rows.push_back({"domains", std::to_string(snapshot.domains.size())});
  rows.push_back({"policies", std::to_string(snapshot.policies.size())});
  rows.push_back({"artifacts", std::to_string(snapshot.artifacts.size())});
  rows.push_back({"workload_classes", std::to_string(snapshot.workload_classes.size())});
  rows.push_back({"workloads", std::to_string(snapshot.workloads.size())});
  rows.push_back({"placements", std::to_string(snapshot.placements.size())});
  rows.push_back({"migrations", std::to_string(snapshot.migrations.size())});
  rows.push_back({"portability", std::to_string(snapshot.portability.size())});
  rows.push_back({"publishers", std::to_string(snapshot.publishers.size())});
  rows.push_back({"truncated", snapshot.truncated ? "yes" : "no"});
  rows.push_back({"records_total", std::to_string(snapshot.records_total)});
  rows.push_back({"records_included", std::to_string(snapshot.records_included)});
  rows.push_back({"precision", std::string(fo::to_string(snapshot.precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(snapshot.evidence_class))});
  std::string out = std::string(indent) + "federation snapshot:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += snapshot.health.render(std::string(indent) + "  ");
  return out;
}

}  // namespace

std::string FederationSnapshot::render(std::string_view indent) const {
  return render_snapshot(*this, true, indent);
}

std::string FederationSnapshot::digest() const {
  return digest_text(render_snapshot(*this, false, ""));
}

}  // namespace fo
