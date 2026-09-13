// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Stranded capacity, federation fragmentation, aggregate capability mismatch, and drift.
// Every finding states its population and its precision; nothing is extrapolated from
// evidence that is not current unless the caller explicitly asks for stale evidence.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/snapshot.hpp"

namespace fo {
namespace {

CapabilitySet merge_capabilities(const ClusterRecord& cluster,
                                 const AcceleratorClassRecord* accelerator_class,
                                 const RuntimeRecord* runtime) {
  // Precedence, weakest evidence first: the accelerator-class catalogue describes the
  // device model, the cluster record describes what this fleet actually publishes today,
  // and the runtime record is authoritative for runtime and driver keys. A later write
  // wins, so a cluster can legitimately withdraw a capability the catalogue advertises.
  CapabilitySet merged;
  merged.set_limit(cluster.capabilities.limit());
  if (accelerator_class != nullptr) {
    for (const CapabilityEntry& entry : accelerator_class->capabilities.entries()) {
      const Status s = merged.put(entry);
      (void)s;
    }
  }
  for (const CapabilityEntry& entry : cluster.capabilities.entries()) {
    const Status s = merged.put(entry);
    (void)s;
  }
  if (runtime != nullptr) {
    for (const CapabilityEntry& entry : runtime->capabilities.entries()) {
      const Status s = merged.put(entry);
      (void)s;
    }
  }
  return merged;
}

StrandingReason reason_for_requirement(CapabilityKey key) {
  switch (key) {
    case CapabilityKey::PrecisionFp64:
    case CapabilityKey::PrecisionFp32:
    case CapabilityKey::PrecisionTf32:
    case CapabilityKey::PrecisionFp16:
    case CapabilityKey::PrecisionBf16:
    case CapabilityKey::PrecisionFp8E4M3:
    case CapabilityKey::PrecisionFp8E5M2:
    case CapabilityKey::PrecisionFp6:
    case CapabilityKey::PrecisionInt8:
    case CapabilityKey::PrecisionInt4:
    case CapabilityKey::PrecisionFp4:
      return StrandingReason::UnsupportedPrecision;
    case CapabilityKey::InterconnectType:
    case CapabilityKey::InterconnectBandwidthGbps:
    case CapabilityKey::NvlinkGeneration:
    case CapabilityKey::PcieGeneration:
      return StrandingReason::MissingInterconnectFeature;
    case CapabilityKey::OffloadCapability:
      return StrandingReason::MissingOffloadCapability;
    case CapabilityKey::PartitionSupport:
    case CapabilityKey::PartitionModes:
    case CapabilityKey::PartitionGeometry:
    case CapabilityKey::PartitionMaxInstances:
      return StrandingReason::PartitionGeometry;
    case CapabilityKey::RuntimeApi:
    case CapabilityKey::RuntimeVersion:
    case CapabilityKey::RuntimeGeneration:
      return StrandingReason::RuntimeMismatch;
    case CapabilityKey::RuntimeAbi:
      return StrandingReason::AbiMismatch;
    case CapabilityKey::DriverApi:
    case CapabilityKey::DriverVersion:
    case CapabilityKey::DriverAbi:
    case CapabilityKey::DriverGeneration:
      return StrandingReason::DriverMismatch;
    case CapabilityKey::CompilerTarget:
    case CapabilityKey::CompilerVersion:
      return StrandingReason::MissingCompilerTarget;
    case CapabilityKey::ArtifactFormat:
    case CapabilityKey::ArtifactFormats:
    case CapabilityKey::KernelFormat:
    case CapabilityKey::KernelArchitectures:
    case CapabilityKey::QuantizationSupport:
    case CapabilityKey::QuantizationFormats:
    case CapabilityKey::SparseExecutionSupport:
    case CapabilityKey::InferenceServingBackend:
      return StrandingReason::ArtifactIncompatibility;
    case CapabilityKey::CollectiveSupport:
    case CapabilityKey::CollectiveBackend:
    case CapabilityKey::CollectiveMaxGroupSize:
      return StrandingReason::TopologyConstraint;
    case CapabilityKey::SecurityIsolation:
      return StrandingReason::IsolationRequirement;
    case CapabilityKey::MemoryBytes:
    case CapabilityKey::MemoryBandwidthGbps:
    case CapabilityKey::MemoryType:
    case CapabilityKey::MemoryEcc:
      return StrandingReason::MemoryInsufficient;
    default:
      return StrandingReason::UnsupportedAcceleratorCapability;
  }
}

bool is_code_bearing(ArtifactKind kind) {
  switch (kind) {
    case ArtifactKind::ExecutableBinary:
    case ArtifactKind::SharedLibrary:
    case ArtifactKind::DeviceImage:
    case ArtifactKind::ContainerImage:
      return true;
    default:
      return false;
  }
}

/// Eligibility of one homogeneous capacity pool for one workload generation.
struct PoolEligibility {
  bool decided = false;      ///< false means the evidence could not decide.
  bool eligible = false;
  StrandingReason primary = StrandingReason::None;
  ReasonTally occurrences;
  std::vector<std::string> notes;
};

void note_reason(PoolEligibility& result, StrandingReason reason) {
  const Status s = result.occurrences.add(reason, 1);
  (void)s;
  if (result.primary == StrandingReason::None ||
      stranding_reason_precedence(reason) < stranding_reason_precedence(result.primary)) {
    result.primary = reason;
  }
}

PoolEligibility evaluate_pool(const WorkloadRecord* workload, const ArtifactRecord* artifact,
                              const ClusterRecord& cluster, const SiteRecord* site,
                              const AcceleratorClassRecord* accelerator_class,
                              const RuntimeRecord* runtime, const PolicyRecord* policy,
                              ResourceKind kind) {
  PoolEligibility result;

  if (cluster.readiness == Readiness::NotReady || cluster.readiness == Readiness::Draining) {
    // A cluster that reports idle capacity while also reporting that it is not ready is
    // internally inconsistent. The runtime does not pick a winner; it declines to decide.
    result.decided = false;
    result.notes.emplace_back(
        "cluster reports idle capacity while its readiness is " +
        std::string(fo::to_string(cluster.readiness)) +
        "; the runtime does not resolve the inconsistency");
    return result;
  }

  if (kind == ResourceKind::Accelerator && accelerator_class == nullptr) {
    result.decided = false;
    result.notes.emplace_back("the accelerator class of this pool is not published");
    return result;
  }

  const CapabilitySet effective = merge_capabilities(cluster, accelerator_class, runtime);
  const CapabilitySet* artifact_capabilities = nullptr;
  CapabilitySet artifact_set;
  if (artifact != nullptr && !artifact->capabilities.empty()) {
    artifact_set = artifact->capabilities;
    artifact_capabilities = &artifact_set;
  }
  (void)artifact_capabilities;

  bool any_failure = false;
  bool any_undecided = false;

  // Artifact architecture.
  if (artifact != nullptr && is_code_bearing(artifact->kind)) {
    if (artifact->target_architectures.empty()) {
      any_undecided = true;
      result.notes.emplace_back("the artifact does not declare its target architectures");
    } else if (accelerator_class == nullptr || accelerator_class->architecture.empty()) {
      any_undecided = true;
      result.notes.emplace_back("the accelerator architecture of this pool is unpublished");
    } else if (!artifact->targets_architecture(accelerator_class->architecture)) {
      any_failure = true;
      note_reason(result, StrandingReason::ArtifactIncompatibility);
      result.notes.emplace_back("the artifact targets " +
                                join_strings(artifact->target_architectures, ",") +
                                " and not the pool architecture " +
                                accelerator_class->architecture);
    }
  }

  // Artifact format / kernel format / state format support.
  const auto require_text = [&](const std::string& value, CapabilityKey key,
                                StrandingReason reason, const char* label) {
    if (value.empty()) {
      return;
    }
    CapabilityRef ref;
    ref.key = key;
    const CapabilityEntry* entry = effective.find(ref);
    if (entry == nullptr || !entry->value.known()) {
      any_undecided = true;
      result.notes.emplace_back(std::string("the target does not declare ") + label);
      return;
    }
    bool supported = false;
    if (entry->value.is_text()) {
      supported = entry->value.as_text() == value;
    } else if (entry->value.is_text_set()) {
      const auto& values = entry->value.as_text_set();
      supported = std::find(values.begin(), values.end(), value) != values.end();
    } else {
      any_undecided = true;
      result.notes.emplace_back(std::string("the declared ") + label + " is not comparable");
      return;
    }
    if (!supported) {
      any_failure = true;
      note_reason(result, reason);
      result.notes.emplace_back(std::string(label) + " " + value + " is not accepted by the target");
    }
  };

  if (artifact != nullptr) {
    require_text(artifact->format, CapabilityKey::ArtifactFormats,
                 StrandingReason::ArtifactIncompatibility, "artifact format");
    require_text(artifact->kernel_format, CapabilityKey::KernelFormat,
                 StrandingReason::ArtifactIncompatibility, "kernel format");
    require_text(artifact->state_format, CapabilityKey::ArtifactFormats,
                 StrandingReason::MissingLocalStorageState, "state format");
    require_text(artifact->compiler_target, CapabilityKey::CompilerTarget,
                 StrandingReason::MissingCompilerTarget, "compiler target");
  }

  // Runtime ABI.
  if (artifact != nullptr && !artifact->runtime_abi.empty()) {
    if (runtime == nullptr || runtime->abi.empty()) {
      any_undecided = true;
      result.notes.emplace_back("the target runtime does not publish an ABI");
    } else if (runtime->abi != artifact->runtime_abi) {
      any_failure = true;
      note_reason(result, StrandingReason::AbiMismatch);
      result.notes.emplace_back("the artifact requires runtime ABI " + artifact->runtime_abi +
                                " but the target publishes " + runtime->abi);
    }
  }

  // Workload-declared requirements.
  if (workload != nullptr) {
    for (const CapabilityRequirement& requirement : workload->requirements) {
      const CapabilityCheck check = evaluate_requirement(requirement, effective);
      if (check.satisfied == Tri::No) {
        if (requirement.optional) {
          continue;
        }
        any_failure = true;
        note_reason(result, reason_for_requirement(requirement.key.key));
        result.notes.emplace_back("unsatisfied requirement " + requirement.to_string());
      } else if (check.satisfied == Tri::Unknown) {
        any_undecided = true;
        result.notes.emplace_back("undecided requirement " + requirement.to_string());
      }
    }
    if (workload->required_memory_bytes_per_accelerator > 0) {
      if (accelerator_class == nullptr || accelerator_class->memory_bytes_per_device == 0) {
        any_undecided = true;
        result.notes.emplace_back("per-device memory is unpublished for this pool");
      } else if (workload->required_memory_bytes_per_accelerator >
                 accelerator_class->memory_bytes_per_device) {
        any_failure = true;
        note_reason(result, StrandingReason::MemoryInsufficient);
        result.notes.emplace_back("the workload requires " +
                                  std::to_string(workload->required_memory_bytes_per_accelerator) +
                                  " bytes per device, the pool provides " +
                                  std::to_string(accelerator_class->memory_bytes_per_device));
      }
    }
  }

  // Policy admissibility.
  if (policy != nullptr) {
    Tri permitted = Tri::Unknown;
    if (site != nullptr) {
      const Tri site_permitted = policy->permits_site(site->id);
      if (site_permitted == Tri::No) {
        permitted = Tri::No;
        note_reason(result, StrandingReason::SiteRestriction);
        any_failure = true;
        result.notes.emplace_back("policy " + policy->id.value() + " generation " +
                                  policy->generation.to_string() + " excludes site " + site->id.value());
      }
    }
    if (permitted != Tri::No) {
      const Tri cluster_permitted = policy->permits_cluster(cluster.id);
      if (cluster_permitted == Tri::No) {
        permitted = Tri::No;
        note_reason(result, StrandingReason::PolicyRestriction);
        any_failure = true;
        result.notes.emplace_back("policy " + policy->id.value() + " generation " +
                                  policy->generation.to_string() + " excludes cluster " +
                                  cluster.id.value());
      } else if (cluster_permitted == Tri::Unknown) {
        permitted = Tri::Unknown;
        any_undecided = true;
        result.notes.emplace_back("policy admissibility of this cluster is undecided");
      }
    }
  }

  if (any_failure) {
    result.decided = true;
    result.eligible = false;
    return result;
  }
  if (any_undecided) {
    result.decided = false;
    return result;
  }
  result.decided = true;
  result.eligible = true;
  return result;
}

const WorkloadRecord* select_workload(const FederationSnapshot& snapshot,
                                      const WorkloadClassId& workload_class) {
  const WorkloadRecord* selected = nullptr;
  for (const WorkloadRecord& workload : snapshot.workloads) {
    if (workload.workload_class != workload_class) {
      continue;
    }
    if (selected == nullptr || workload.id < selected->id) {
      selected = &workload;
    }
  }
  return selected;
}

const RuntimeRecord* primary_runtime(const FederationSnapshot& snapshot,
                                     const ClusterRecord& cluster) {
  for (const RuntimeId& id : cluster.runtimes) {
    if (const RuntimeRecord* runtime = snapshot.find_runtime(id)) {
      return runtime;
    }
  }
  return nullptr;
}

}  // namespace

bool operator<(const StrandedCapacityFinding& a, const StrandedCapacityFinding& b) noexcept {
  if (a.cluster != b.cluster) return a.cluster < b.cluster;
  if (a.pool != b.pool) return a.pool < b.pool;
  return a.kind < b.kind;
}

std::string StrandedCapacityFinding::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", federation.value()});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"workload_class", workload_class.value()});
  rows.push_back({"cluster", cluster.value()});
  rows.push_back({"cluster_generation", cluster_generation.is_set() ? cluster_generation.to_string() : "-"});
  rows.push_back({"cluster_epoch", cluster_epoch.is_set() ? cluster_epoch.to_string() : "-"});
  rows.push_back({"site", render_id(site.value())});
  rows.push_back({"accelerator_class", render_id(accelerator_class.value())});
  rows.push_back({"pool", render_id(pool.value())});
  rows.push_back({"resource_kind", std::string(fo::to_string(kind))});
  rows.push_back({"capacity_generation", capacity_generation.is_set() ? capacity_generation.to_string() : "-"});
  rows.push_back({"capability_generation",
                  capability_generation.is_set() ? capability_generation.to_string() : "-"});
  rows.push_back({"topology_generation",
                  topology_generation.is_set() ? topology_generation.to_string() : "-"});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = std::string(indent) + "stranded_capacity_finding:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += ledger.render(std::string(indent) + "  ");
  out += '\n';
  out += breakdown.render(std::string(indent) + "  ");
  return out;
}

bool StrandedCapacityReport::closes() const noexcept {
  std::uint64_t expected = 0;
  if (!checked_add(summary.usable, summary.stranded, expected)) {
    return false;
  }
  if (!checked_add(expected, summary.unknown, expected)) {
    return false;
  }
  return expected == summary.idle;
}

std::string StrandedCapacityReport::digest() const { return digest_text(render()); }

std::string StrandedCapacityReport::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", render_id(federation.value())});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"coordinator_epoch",
                  coordinator_epoch.is_set() ? coordinator_epoch.to_string() : "-"});
  rows.push_back({"snapshot_generation",
                  snapshot_generation.is_set() ? snapshot_generation.to_string() : "-"});
  rows.push_back({"workload_class", render_id(workload_class.value())});
  rows.push_back({"resource_kind", std::string(fo::to_string(kind))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"accounting_closes", closes() ? "yes" : "no"});
  rows.push_back({"excluded_stale_clusters", std::to_string(excluded_stale_clusters.size())});
  rows.push_back({"excluded_unknown_clusters", std::to_string(excluded_unknown_clusters.size())});
  std::string out = "stranded capacity report:\n";
  out += render_table({"field", "value"}, rows, "  ");
  out += '\n';
  out += summary.render("  ");
  out += "\n  findings (per cluster and pool):";
  if (findings.empty()) {
    out += "\n    <none>";
  }
  for (const StrandedCapacityFinding& finding : findings) {
    out += '\n';
    out += finding.render("    ");
  }
  if (!excluded_stale_clusters.empty()) {
    out += "\n  excluded because their evidence is not current:";
    for (const ClusterId& cluster : excluded_stale_clusters) {
      out += "\n    - ";
      out += cluster.value();
      out += " (capacity is NOT counted in the totals above)";
    }
  }
  if (!excluded_unknown_clusters.empty()) {
    out += "\n  excluded because no capacity evidence was published:";
    for (const ClusterId& cluster : excluded_unknown_clusters) {
      out += "\n    - ";
      out += cluster.value();
    }
  }
  return out;
}

Result<StrandedCapacityReport> analyze_stranded_capacity(const FederationSnapshot& snapshot,
                                                         const StrandedCapacityRequest& request) {
  StrandedCapacityReport report;
  report.federation = request.federation;
  report.federation_generation = snapshot.generation;
  report.coordinator_epoch = snapshot.coordinator_epoch;
  report.snapshot_generation = snapshot.snapshot_generation;
  report.workload_class = request.workload_class;
  report.kind = request.kind;

  const WorkloadRecord* workload = select_workload(snapshot, request.workload_class);
  if (workload == nullptr && !request.workload_class.empty()) {
    return Error(ErrorCode::NotFound, "no workload generation is registered for this workload class",
                 request.workload_class.value());
  }
  const ArtifactRecord* artifact =
      workload != nullptr ? snapshot.find_artifact(workload->artifact) : nullptr;
  const PolicyRecord* policy =
      workload != nullptr && !workload->policy.empty() ? snapshot.find_policy(workload->policy) : nullptr;

  Precision precision = Precision::Exact;
  EvidenceClass evidence_class = EvidenceClass::Real;
  bool first = true;

  for (const ClusterRecord& cluster : snapshot.clusters) {
    if (!request.federation.empty() && cluster.federation != request.federation) {
      continue;
    }
    if (!request.cluster.empty() && cluster.id != request.cluster) {
      continue;
    }
    if (cluster.currentness == Currentness::Retired) {
      continue;
    }
    if (cluster.currentness != Currentness::Current && !request.include_stale) {
      report.excluded_stale_clusters.push_back(cluster.id);
      continue;
    }
    const SiteRecord* site = snapshot.find_site(cluster.site);
    bool any_pool = false;
    for (const CapacityPool& pool : cluster.capacity_pools) {
      if (pool.kind != request.kind) {
        continue;
      }
      if (!request.accelerator_class.empty() && pool.accelerator_class != request.accelerator_class) {
        continue;
      }
      any_pool = true;
      const AcceleratorClassRecord* accelerator_class =
          pool.accelerator_class.empty() ? nullptr : snapshot.find_accelerator_class(pool.accelerator_class);
      const RuntimeRecord* runtime = primary_runtime(snapshot, cluster);

      StrandedCapacityFinding finding;
      finding.federation = cluster.federation;
      finding.federation_generation = snapshot.generation;
      finding.workload_class = request.workload_class;
      finding.cluster = cluster.id;
      finding.cluster_generation = cluster.generation;
      finding.cluster_epoch = cluster.epoch;
      finding.site = cluster.site;
      finding.accelerator_class = pool.accelerator_class;
      finding.pool = pool.pool_id;
      finding.kind = pool.kind;
      finding.capacity_generation = pool.generation;
      finding.capability_generation = cluster.capability_generation;
      finding.topology_generation = cluster.topology_generation;
      finding.compatibility_generation = cluster.compatibility_generation;
      finding.ledger = pool.ledger;
      finding.currentness = cluster.currentness;
      finding.precision = weakest(pool.precision, cluster.stamp.precision);
      finding.evidence_class = weaker(pool.evidence_class, cluster.stamp.evidence_class);

      const PoolEligibility eligibility =
          evaluate_pool(workload, artifact, cluster, site, accelerator_class, runtime, policy, pool.kind);

      finding.breakdown.idle = pool.ledger.idle;
      if (eligibility.decided && eligibility.eligible) {
        finding.breakdown.usable = pool.ledger.idle;
      } else if (!eligibility.decided) {
        finding.breakdown.unknown = pool.ledger.idle;
      } else {
        const Status s =
            finding.breakdown.stranded_by_primary_reason.add(eligibility.primary, pool.ledger.idle);
        if (!s.ok()) {
          return s.error();
        }
        for (const StrandingReason reason : [&] {
               std::vector<StrandingReason> reasons;
               for (const auto& entry : eligibility.occurrences.non_zero()) {
                 reasons.push_back(entry.first);
               }
               return reasons;
             }()) {
          const Status add_status =
              finding.breakdown.reason_occurrences.add(reason, pool.ledger.idle);
          if (!add_status.ok()) {
            return add_status.error();
          }
        }
      }
      for (const std::string& note : eligibility.notes) {
        const Status s = finding.evidence.add(Provenance::DerivedAnalysis, finding.evidence_class,
                                              "stranding-analysis", Precision::Derived, note);
        (void)s;
      }

      std::uint64_t value = 0;
      if (!checked_add(report.summary.nominal, pool.ledger.nominal, value)) {
        return Error(ErrorCode::CapacityInconsistent, "stranded report nominal overflowed");
      }
      report.summary.nominal = value;
      report.summary.offline = saturating_add(report.summary.offline, pool.ledger.offline);
      report.summary.allocated = saturating_add(report.summary.allocated, pool.ledger.allocated);
      report.summary.reserved = saturating_add(report.summary.reserved, pool.ledger.reserved);
      report.summary.draining = saturating_add(report.summary.draining, pool.ledger.draining);
      report.summary.unusable = saturating_add(report.summary.unusable, pool.ledger.unusable);
      report.summary.idle = saturating_add(report.summary.idle, pool.ledger.idle);
      report.summary.usable = saturating_add(report.summary.usable, finding.breakdown.usable);
      report.summary.stranded = saturating_add(report.summary.stranded, finding.breakdown.stranded());
      report.summary.unknown = saturating_add(report.summary.unknown, finding.breakdown.unknown);

      precision = first ? finding.precision : weakest(precision, finding.precision);
      evidence_class = first ? finding.evidence_class : weaker(evidence_class, finding.evidence_class);
      first = false;
      report.findings.push_back(std::move(finding));
    }
    if (!any_pool) {
      report.excluded_unknown_clusters.push_back(cluster.id);
    }
  }

  if (first) {
    precision = Precision::Unknown;
    evidence_class = snapshot.evidence_class;
  }
  report.precision = precision;
  report.evidence_class = evidence_class;

  std::sort(report.findings.begin(), report.findings.end());
  const Status s = report.evidence.add(
      Provenance::DerivedAnalysis, evidence_class, "stranded-capacity-analysis", Precision::Derived,
      "partition of idle capacity for workload class " + render_id(request.workload_class.value()) +
          " over " + std::to_string(report.findings.size()) + " capacity pools");
  (void)s;
  if (!report.closes()) {
    return Error(ErrorCode::CapacityInconsistent,
                 "stranded capacity report does not close",
                 "nominal=" + std::to_string(report.summary.nominal) +
                     " idle=" + std::to_string(report.summary.idle) +
                     " usable=" + std::to_string(report.summary.usable) +
                     " stranded=" + std::to_string(report.summary.stranded) +
                     " unknown=" + std::to_string(report.summary.unknown));
  }
  return report;
}

bool operator<(const DomainCapacity& a, const DomainCapacity& b) noexcept {
  return a.domain < b.domain;
}

// NOTE: digest() is the fingerprint of render(), so render() must never contain the
// digest. Including it would recurse without bound; the codec round-trip and message
// rendering tests guard the rest of the encoding surface.
std::string FragmentationFinding::digest() const { return digest_text(render()); }

std::string FragmentationFinding::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", federation.value()});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"workload_class", workload_class.value()});
  rows.push_back({"resource_kind", std::string(fo::to_string(kind))});
  rows.push_back({"required_domain_kind", std::string(fo::to_string(required_domain_kind))});
  rows.push_back({"required_per_group", std::to_string(required_per_group)});
  rows.push_back({"aggregate_nominal", std::to_string(aggregate_nominal)});
  rows.push_back({"aggregate_usable", std::to_string(aggregate_usable)});
  rows.push_back({"aggregate_stranded", std::to_string(aggregate_stranded)});
  rows.push_back({"aggregate_unknown", std::to_string(aggregate_unknown)});
  rows.push_back({"largest_legal_group", std::to_string(largest_legal_group)});
  rows.push_back({"largest_domain", render_id(largest_domain.value())});
  rows.push_back({"legal_domain_count", std::to_string(legal_domain_count)});
  rows.push_back({"classification", std::string(fo::to_string(classification))});
  rows.push_back({"capacity_generation", capacity_generation.is_set() ? capacity_generation.to_string() : "-"});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = std::string(indent) + "federation_fragmentation:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  if (!domains.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> domain_rows;
    for (const DomainCapacity& domain : domains) {
      domain_rows.push_back({domain.domain.value(), std::string(fo::to_string(domain.kind)),
                             render_id(domain.site.value()), std::to_string(domain.nominal),
                             std::to_string(domain.usable), std::to_string(domain.stranded),
                             std::to_string(domain.unknown),
                             std::to_string(domain.clusters.size())});
    }
    out += render_table({"domain", "kind", "site", "nominal", "usable", "stranded", "unknown", "clusters"},
                        domain_rows, std::string(indent) + "  ");
    out += '\n';
  }
  const auto reasons = stranded_by_reason.non_zero();
  if (!reasons.empty()) {
    std::vector<std::vector<std::string>> reason_rows;
    for (const auto& entry : reasons) {
      reason_rows.push_back({std::string(fo::to_string(entry.first)), std::to_string(entry.second),
                             is_technical_reason(entry.first) ? "technical" : "policy"});
    }
    out += render_table({"stranding_reason", "amount", "nature"}, reason_rows,
                        std::string(indent) + "  ");
    out += '\n';
  }
  return out;
}

Result<FragmentationFinding> analyze_fragmentation(const FederationSnapshot& snapshot,
                                                   const FederationId& federation,
                                                   const WorkloadClassId& workload_class,
                                                   ResourceKind kind) {
  FragmentationFinding finding;
  finding.federation = federation.empty() ? snapshot.federation : federation;
  finding.federation_generation = snapshot.generation;
  finding.workload_class = workload_class;
  finding.kind = kind;

  const WorkloadRecord* workload = select_workload(snapshot, workload_class);
  if (workload == nullptr) {
    return Error(ErrorCode::NotFound, "no workload generation is registered for this workload class",
                 workload_class.value());
  }
  finding.required_per_group = workload->required_accelerators;
  finding.required_domain_kind = workload->required_domain_kind;

  // Determine the domain grouping. Explicitly registered domains win; a cluster that
  // publishes no domain is treated as its own cluster-scoped placement domain, which is
  // stated in the evidence rather than assumed silently.
  std::map<DomainId, DomainCapacity> domain_map;
  bool synthesized_domains = false;
  for (const DomainRecord& domain : snapshot.domains) {
    if (!finding.federation.empty() && domain.federation != finding.federation) {
      continue;
    }
    DomainCapacity capacity;
    capacity.domain = domain.id;
    capacity.kind = domain.kind;
    capacity.site = domain.site;
    capacity.clusters = domain.clusters;
    domain_map[domain.id] = capacity;
  }

  StrandedCapacityRequest request;
  request.federation = finding.federation;
  request.workload_class = workload_class;
  request.kind = kind;
  const Result<StrandedCapacityReport> report = analyze_stranded_capacity(snapshot, request);
  if (!report.ok()) {
    return report.error();
  }

  for (const StrandedCapacityFinding& row : report.value().findings) {
    DomainId domain_id;
    DomainKind domain_kind = DomainKind::Cluster;
    const ClusterRecord* cluster = snapshot.find_cluster(row.cluster);
    if (cluster != nullptr && !cluster->domain.empty()) {
      domain_id = cluster->domain;
      const DomainRecord* record = snapshot.find_domain(cluster->domain);
      domain_kind = record != nullptr ? record->kind : DomainKind::Cluster;
    } else {
      domain_id = DomainId::unchecked(row.cluster.value());
      synthesized_domains = true;
    }
    auto it = domain_map.find(domain_id);
    if (it == domain_map.end()) {
      DomainCapacity capacity;
      capacity.domain = domain_id;
      capacity.kind = domain_kind;
      capacity.site = row.site;
      it = domain_map.emplace(domain_id, capacity).first;
    }
    DomainCapacity& capacity = it->second;
    capacity.nominal = saturating_add(capacity.nominal, row.ledger.nominal);
    capacity.usable = saturating_add(capacity.usable, row.breakdown.usable);
    capacity.stranded = saturating_add(capacity.stranded, row.breakdown.stranded());
    capacity.unknown = saturating_add(capacity.unknown, row.breakdown.unknown);
    if (std::find(capacity.clusters.begin(), capacity.clusters.end(), row.cluster) ==
        capacity.clusters.end()) {
      capacity.clusters.push_back(row.cluster);
      std::sort(capacity.clusters.begin(), capacity.clusters.end());
    }
    for (const auto& entry : row.breakdown.stranded_by_primary_reason.non_zero()) {
      const Status s = finding.stranded_by_reason.add(entry.first, entry.second);
      if (!s.ok()) {
        return s.error();
      }
    }
  }

  for (auto& entry : domain_map) {
    DomainCapacity& capacity = entry.second;
    finding.aggregate_nominal = saturating_add(finding.aggregate_nominal, capacity.nominal);
    finding.aggregate_usable = saturating_add(finding.aggregate_usable, capacity.usable);
    finding.aggregate_stranded = saturating_add(finding.aggregate_stranded, capacity.stranded);
    finding.aggregate_unknown = saturating_add(finding.aggregate_unknown, capacity.unknown);
    if (capacity.usable > finding.largest_legal_group) {
      finding.largest_legal_group = capacity.usable;
      finding.largest_domain = capacity.domain;
    }
    if (finding.required_per_group != 0 && capacity.usable >= finding.required_per_group) {
      finding.legal_domain_count += 1;
    }
    finding.domains.push_back(capacity);
  }
  std::sort(finding.domains.begin(), finding.domains.end());

  // Classification. Capacity shortage and fragmentation are different findings: the
  // first means the federation does not have the capacity, the second means it has the
  // capacity but not where it can legally be used together.
  bool policy_reasons = false;
  bool topology_reasons = false;
  bool compatibility_reasons = false;
  for (const auto& entry : finding.stranded_by_reason.non_zero()) {
    switch (entry.first) {
      case StrandingReason::SiteRestriction:
      case StrandingReason::PolicyRestriction:
        policy_reasons = true;
        break;
      case StrandingReason::TopologyConstraint:
      case StrandingReason::PartitionGeometry:
      case StrandingReason::MissingInterconnectFeature:
        topology_reasons = true;
        break;
      case StrandingReason::None:
      case StrandingReason::Unknown:
      case StrandingReason::IsolationRequirement:
      case StrandingReason::MissingLocalStorageState:
      case StrandingReason::MemoryInsufficient:
        break;
      default:
        compatibility_reasons = true;
        break;
    }
  }

  if (finding.required_per_group == 0) {
    finding.classification = FragmentationClass::Unknown;
  } else if (finding.aggregate_usable < finding.required_per_group) {
    finding.classification = FragmentationClass::CapacityShortage;
  } else if (finding.largest_legal_group >= finding.required_per_group) {
    finding.classification = FragmentationClass::None;
  } else if (finding.required_domain_kind == DomainKind::Federation && policy_reasons) {
    finding.classification = FragmentationClass::SiteFragmentation;
  } else if (policy_reasons && (compatibility_reasons || topology_reasons)) {
    finding.classification = FragmentationClass::Mixed;
  } else if (policy_reasons) {
    finding.classification = FragmentationClass::PolicyFragmentation;
  } else if (compatibility_reasons && topology_reasons) {
    finding.classification = FragmentationClass::Mixed;
  } else if (compatibility_reasons) {
    finding.classification = FragmentationClass::CompatibilityFragmentation;
  } else if (topology_reasons) {
    finding.classification = FragmentationClass::TopologyFragmentation;
  } else {
    finding.classification = FragmentationClass::PhysicalFragmentation;
  }

  finding.capacity_generation = snapshot.clusters.empty()
                                    ? CapacityGeneration{}
                                    : snapshot.clusters.front().capacity_generation;
  finding.precision = report.value().precision;
  finding.evidence_class = report.value().evidence_class;
  finding.currentness = SnapshotGeneration{} == snapshot.snapshot_generation
                            ? Currentness::Unknown
                            : Currentness::Current;
  for (const EvidenceRef& ref : report.value().evidence.items()) {
    const Status s = finding.evidence.add(ref);
    (void)s;
  }
  const Status s = finding.evidence.add(
      Provenance::DerivedAnalysis, finding.evidence_class, "fragmentation-analysis",
      Precision::Derived,
      std::string("grouped usable capacity into ") + std::to_string(finding.domains.size()) +
          " placement domains" +
          (synthesized_domains
               ? "; clusters that publish no domain were treated as their own cluster-scoped domain"
               : ""));
  (void)s;
  return finding;
}

bool operator<(const MismatchRow& a, const MismatchRow& b) noexcept { return a.subject < b.subject; }

std::string MismatchRow::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"subject", subject});
  rows.push_back({"population", std::to_string(population)});
  rows.push_back({"satisfied", std::to_string(satisfied)});
  rows.push_back({"missing", std::to_string(missing)});
  rows.push_back({"unknown", std::to_string(unknown)});
  rows.push_back({"missing_percent", percent_string(missing, population)});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  return render_table({"field", "value"}, rows, indent);
}

std::string MismatchAnalysis::digest() const { return digest_text(render()); }

std::string MismatchAnalysis::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", render_id(federation.value())});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"snapshot_generation",
                  snapshot_generation.is_set() ? snapshot_generation.to_string() : "-"});
  rows.push_back({"window", window.render()});
  rows.push_back({"placements_observed", std::to_string(placements_observed)});
  rows.push_back({"migrations_observed", std::to_string(migrations_observed)});
  rows.push_back({"portability_records_observed", std::to_string(portability_records_observed)});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = "capability mismatch analysis:\n";
  out += render_table({"field", "value"}, rows, "  ");
  out += "\n\n  NOTE: \"missing\" means the publisher did not declare the capability for that\n";
  out += "  subject. It is a statement about the published evidence, not about the hardware.\n";
  out += "  \"unknown\" means the published evidence does not decide the question.\n";

  const auto section = [&out](const char* title,
                              const std::vector<MismatchRow>& table) {
    out += "\n  ";
    out += title;
    out += ":";
    if (table.empty()) {
      out += " <none>";
      return;
    }
    std::vector<std::vector<std::string>> table_rows;
    for (const MismatchRow& row : table) {
      table_rows.push_back({row.subject, std::to_string(row.population),
                            std::to_string(row.satisfied), std::to_string(row.missing),
                            std::to_string(row.unknown), percent_string(row.missing, row.population)});
    }
    out += '\n';
    out += render_table({"subject", "population", "satisfied", "missing", "unknown", "missing_percent"},
                        table_rows, "    ");
  };

  section("by capability key", by_capability_key);
  section("by accelerator class", by_accelerator_class);
  section("by cluster", by_cluster);
  section("by site", by_site);
  section("by runtime", by_runtime);
  section("by workload class", by_workload_class);
  section("by artifact", by_artifact);
  section("placement rejections", placement_rejections);
  section("capacity stranded by reason", capacity_stranded_by_reason);
  section("migration adaptations", migration_adaptations);
  section("portability outcomes", portability_outcomes);
  return out;
}

namespace {

MismatchRow* find_or_add(std::vector<MismatchRow>& rows, const std::string& subject) {
  for (MismatchRow& row : rows) {
    if (row.subject == subject) {
      return &row;
    }
  }
  MismatchRow row;
  row.subject = subject;
  rows.push_back(row);
  return &rows.back();
}

void bump(std::vector<MismatchRow>& rows, const std::string& subject, Tri satisfied,
          Precision precision) {
  MismatchRow* row = find_or_add(rows, subject);
  row->population += 1;
  switch (satisfied) {
    case Tri::Yes: row->satisfied += 1; break;
    case Tri::No: row->missing += 1; break;
    case Tri::Unknown: row->unknown += 1; break;
  }
  row->precision = weakest(row->precision, precision);
}

}  // namespace

Result<MismatchAnalysis> analyze_mismatch(const FederationSnapshot& snapshot,
                                          const MismatchAnalysisRequest& request) {
  MismatchAnalysis analysis;
  analysis.federation = request.federation;
  analysis.federation_generation = snapshot.generation;
  analysis.snapshot_generation = snapshot.snapshot_generation;
  analysis.window = request.window;

  // Population: clusters whose evidence participates.
  std::vector<const ClusterRecord*> clusters;
  for (const ClusterRecord& cluster : snapshot.clusters) {
    if (!request.federation.empty() && cluster.federation != request.federation) {
      continue;
    }
    if (cluster.currentness == Currentness::Retired) {
      continue;
    }
    if (cluster.currentness != Currentness::Current && !request.include_stale) {
      continue;
    }
    clusters.push_back(&cluster);
  }

  // Capability keys observed anywhere, in deterministic key order.
  std::vector<CapabilityRef> keys;
  const auto collect = [&keys](const CapabilitySet& set) {
    for (const CapabilityEntry& entry : set.entries()) {
      if (std::find(keys.begin(), keys.end(), entry.key) == keys.end()) {
        keys.push_back(entry.key);
      }
    }
  };
  for (const ClusterRecord* cluster : clusters) {
    collect(cluster->capabilities);
  }
  for (const AcceleratorClassRecord& accelerator_class : snapshot.accelerator_classes) {
    collect(accelerator_class.capabilities);
  }
  for (const RuntimeRecord& runtime : snapshot.runtimes) {
    collect(runtime.capabilities);
  }
  std::sort(keys.begin(), keys.end());

  for (const ClusterRecord* cluster : clusters) {
    const AcceleratorClassRecord* accelerator_class = nullptr;
    for (const AcceleratorClassId& id : cluster->accelerator_classes) {
      accelerator_class = snapshot.find_accelerator_class(id);
      if (accelerator_class != nullptr) {
        break;
      }
    }
    const RuntimeRecord* runtime = primary_runtime(snapshot, *cluster);
    const CapabilitySet effective = merge_capabilities(*cluster, accelerator_class, runtime);
    const SiteRecord* site = snapshot.find_site(cluster->site);
    const std::string cluster_subject = cluster->id.value();
    const std::string site_subject = site != nullptr ? site->id.value() : std::string("<no-site>");
    const std::string runtime_subject =
        runtime != nullptr ? runtime->id.value() : std::string("<no-runtime>");
    const std::string accelerator_subject =
        accelerator_class != nullptr ? accelerator_class->id.value() : std::string("<no-class>");

    std::size_t declared = 0;
    for (const CapabilityRef& key : keys) {
      if (!request.capability_key.valid() || request.capability_key == key) {
        // "missing" here means "not declared by the publisher for this subject".
        CapabilityRequirement requirement;
        requirement.key = key;
        requirement.comparator = CapabilityComparator::Present;
        const CapabilityCheck check = evaluate_requirement(requirement, effective);
        const Tri satisfied = check.satisfied == Tri::Yes ? Tri::Yes
                              : check.satisfied == Tri::Unknown ? Tri::Unknown
                                                                : Tri::No;
        bump(analysis.by_capability_key, key.to_string(), satisfied, Precision::Derived);
      }
      CapabilityRef probe;
      probe.key = key.key;
      probe.custom = key.custom;
      if (effective.contains(probe)) {
        ++declared;
      }
    }
    // Per-subject declaration coverage, expressed against the observed key universe.
    const Tri coverage = keys.empty()
                             ? Tri::Unknown
                             : (declared == keys.size() ? Tri::Yes : Tri::No);
    bump(analysis.by_cluster, cluster_subject, coverage, Precision::Derived);
    bump(analysis.by_site, site_subject, coverage, Precision::Derived);
    bump(analysis.by_runtime, runtime_subject, coverage, Precision::Derived);
    bump(analysis.by_accelerator_class, accelerator_subject, coverage, Precision::Derived);
  }

  // Workload-class and artifact rows: how many workload generations of each class can
  // be satisfied by each cluster's declared evidence.
  for (const WorkloadRecord& workload : snapshot.workloads) {
    std::size_t evaluated = 0;
    std::size_t satisfied_clusters = 0;
    std::size_t unknown_clusters = 0;
    for (const ClusterRecord* cluster : clusters) {
      const AcceleratorClassRecord* accelerator_class = nullptr;
      for (const AcceleratorClassId& id : cluster->accelerator_classes) {
        accelerator_class = snapshot.find_accelerator_class(id);
        if (accelerator_class != nullptr) {
          break;
        }
      }
      const RuntimeRecord* runtime = primary_runtime(snapshot, *cluster);
      const CapabilitySet effective = merge_capabilities(*cluster, accelerator_class, runtime);
      ++evaluated;
      bool failed = false;
      bool undecided = false;
      for (const CapabilityRequirement& requirement : workload.requirements) {
        const CapabilityCheck check = evaluate_requirement(requirement, effective);
        if (check.satisfied == Tri::No) {
          failed = true;
          break;
        }
        if (check.satisfied == Tri::Unknown) {
          undecided = true;
        }
      }
      if (failed) {
        continue;
      }
      if (undecided) {
        ++unknown_clusters;
        continue;
      }
      ++satisfied_clusters;
    }
    if (evaluated == 0) {
      continue;
    }
    const Tri outcome = satisfied_clusters == evaluated ? Tri::Yes
                        : (satisfied_clusters + unknown_clusters == evaluated && unknown_clusters > 0)
                            ? Tri::Unknown
                            : Tri::No;
    bump(analysis.by_workload_class, workload.workload_class.value(), outcome, Precision::Derived);
    if (!workload.artifact.empty()) {
      bump(analysis.by_artifact, workload.artifact.value(), outcome, Precision::Derived);
    }
  }

  // Placement rejections inside the window.
  for (const PlacementRecord& placement : snapshot.placements) {
    if (!request.federation.empty() && placement.federation != request.federation) {
      continue;
    }
    if (!request.window.contains(placement.stamp.observed_at)) {
      continue;
    }
    analysis.placements_observed += 1;
    for (const CandidateObservation& candidate : placement.candidates) {
      if (candidate.status != CandidateStatus::Rejected &&
          candidate.status != CandidateStatus::NotConsidered) {
        continue;
      }
      for (const RejectionReason reason : candidate.reason_list()) {
        bump(analysis.placement_rejections, std::string(fo::to_string(reason)), Tri::Yes,
             Precision::Derived);
      }
    }
    if (placement.fallback_required) {
      bump(analysis.placement_rejections, "FALLBACK_REQUIRED", Tri::Yes, Precision::Derived);
    }
  }

  // Capacity stranded by reason, per workload class.
  for (const WorkloadClassRecord& workload_class : snapshot.workload_classes) {
    if (workload_class.currentness == Currentness::Retired) {
      continue;
    }
    StrandedCapacityRequest stranded_request;
    stranded_request.federation = request.federation;
    stranded_request.workload_class = workload_class.id;
    stranded_request.kind = request.kind;
    stranded_request.include_stale = request.include_stale;
    const Result<StrandedCapacityReport> report = analyze_stranded_capacity(snapshot, stranded_request);
    if (!report.ok()) {
      continue;
    }
    for (const StrandedCapacityFinding& finding : report.value().findings) {
      for (const auto& entry : finding.breakdown.stranded_by_primary_reason.non_zero()) {
        MismatchRow* row =
            find_or_add(analysis.capacity_stranded_by_reason, std::string(fo::to_string(entry.first)));
        row->population = saturating_add(row->population, finding.ledger.nominal);
        row->missing = saturating_add(row->missing, entry.second);
        row->precision = Precision::Derived;
      }
    }
  }

  // Migration adaptations and portability outcomes in the window.
  for (const MigrationRecord& migration : snapshot.migrations) {
    if (!request.federation.empty() && migration.federation != request.federation) {
      continue;
    }
    if (!request.window.contains(migration.stamp.observed_at)) {
      continue;
    }
    analysis.migrations_observed += 1;
    if (migration.requires_rebuild) {
      bump(analysis.migration_adaptations, "REQUIRES_REBUILD", Tri::Yes, Precision::Derived);
    }
    if (migration.requires_recompile) {
      bump(analysis.migration_adaptations, "REQUIRES_RECOMPILE", Tri::Yes, Precision::Derived);
    }
    if (migration.requires_conversion) {
      bump(analysis.migration_adaptations, "REQUIRES_CONVERSION", Tri::Yes, Precision::Derived);
    }
    if (migration.requires_state_translation) {
      bump(analysis.migration_adaptations, "REQUIRES_STATE_TRANSLATION", Tri::Yes, Precision::Derived);
    }
    if (migration.revalidation_pending) {
      bump(analysis.migration_adaptations, "REVALIDATION_PENDING", Tri::Yes, Precision::Derived);
    }
    if (migration.outcome == MigrationOutcome::Failed) {
      bump(analysis.migration_adaptations, "MIGRATION_FAILED", Tri::Yes, Precision::Derived);
    }
    if (migration.outcome == MigrationOutcome::RolledBack) {
      bump(analysis.migration_adaptations, "MIGRATION_ROLLED_BACK", Tri::Yes, Precision::Derived);
    }
  }

  for (const PortabilityAssessment& assessment : snapshot.portability) {
    if (!request.federation.empty() && assessment.federation != request.federation) {
      continue;
    }
    if (!request.window.contains(assessment.stamp.observed_at)) {
      continue;
    }
    analysis.portability_records_observed += 1;
    MismatchRow* row =
        find_or_add(analysis.portability_outcomes, std::string(fo::to_string(assessment.overall)));
    row->population += 1;
    if (is_technical_failure(assessment.overall)) {
      row->missing += 1;
    } else if (is_undecided(assessment.overall)) {
      row->unknown += 1;
    } else {
      row->satisfied += 1;
    }
    row->precision = weakest(row->precision, assessment.precision);
  }

  const auto sort_rows = [](std::vector<MismatchRow>& rows) {
    for (MismatchRow& row : rows) {
      if (row.precision == Precision::Exact && row.population != 0) {
        row.precision = Precision::Derived;
      }
    }
    std::sort(rows.begin(), rows.end());
  };
  sort_rows(analysis.by_capability_key);
  sort_rows(analysis.by_accelerator_class);
  sort_rows(analysis.by_cluster);
  sort_rows(analysis.by_site);
  sort_rows(analysis.by_runtime);
  sort_rows(analysis.by_workload_class);
  sort_rows(analysis.by_artifact);
  sort_rows(analysis.placement_rejections);
  sort_rows(analysis.capacity_stranded_by_reason);
  sort_rows(analysis.migration_adaptations);
  sort_rows(analysis.portability_outcomes);

  analysis.precision = Precision::Derived;
  analysis.evidence_class = clusters.empty() ? snapshot.evidence_class
                                             : clusters.front()->stamp.evidence_class;
  const Status s = analysis.evidence.add(
      Provenance::DerivedAnalysis, analysis.evidence_class, "mismatch-analysis", Precision::Derived,
      "aggregated mismatch over " + std::to_string(clusters.size()) +
          " clusters with current evidence");
  (void)s;
  return analysis;
}

bool operator<(const DriftFinding& a, const DriftFinding& b) noexcept {
  if (a.kind != b.kind) return a.kind < b.kind;
  if (a.subject != b.subject) return a.subject < b.subject;
  return a.observed < b.observed;
}

std::string DriftFinding::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"kind", std::string(fo::to_string(kind))});
  rows.push_back({"subject", subject});
  rows.push_back({"intended", intended.empty() ? "-" : intended});
  rows.push_back({"observed", observed.empty() ? "-" : observed});
  rows.push_back({"basis", std::string(fo::to_string(basis))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  return std::string(indent) + "drift_finding:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

std::uint64_t DriftReport::count_of(DriftKind kind) const noexcept {
  std::uint64_t count = 0;
  for (const DriftFinding& finding : findings) {
    if (finding.kind == kind) {
      ++count;
    }
  }
  return count;
}

std::string DriftReport::digest() const { return digest_text(render()); }

std::string DriftReport::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"federation", render_id(federation.value())});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"snapshot_generation",
                  snapshot_generation.is_set() ? snapshot_generation.to_string() : "-"});
  rows.push_back({"intended_state_supplied", intended_state_supplied ? "yes" : "no"});
  rows.push_back({"behavior_window_supplied", behavior_window_supplied ? "yes" : "no"});
  rows.push_back({"before_window", before_window.render()});
  rows.push_back({"after_window", after_window.render()});
  rows.push_back({"findings", std::to_string(findings.size())});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = "drift report:\n";
  out += render_table({"field", "value"}, rows, "  ");
  if (!intended_state_supplied) {
    out += "\n  intended-state drift was NOT evaluated: no intended federation state was\n";
    out += "  supplied. This is not a clean bill of health.";
  }
  if (!behavior_window_supplied) {
    out += "\n  behavioral placement drift was NOT evaluated: no observation windows were\n";
    out += "  supplied.";
  }
  out += "\n  findings:";
  if (findings.empty()) {
    out += " <none>";
  }
  for (const DriftFinding& finding : findings) {
    out += '\n';
    out += finding.render("    ");
  }
  return out;
}

Result<DriftReport> analyze_drift(const FederationSnapshot& snapshot, const DriftRequest& request) {
  DriftReport report;
  report.federation = request.federation.empty() ? snapshot.federation : request.federation;
  report.federation_generation = snapshot.generation;
  report.snapshot_generation = snapshot.snapshot_generation;
  report.intended_state_supplied = request.intended != nullptr;
  report.behavior_window_supplied = request.behavior_window_supplied;
  report.before_window = request.before_window;
  report.after_window = request.after_window;

  if (request.intended != nullptr) {
    const IntendedFederationState& intended = *request.intended;
    const Status valid = intended.validate();
    if (!valid.ok()) {
      return valid.error();
    }
    for (const ClusterId& expected : intended.expected_clusters) {
      const ClusterRecord* cluster = snapshot.find_cluster(expected);
      if (cluster == nullptr) {
        DriftFinding finding;
        finding.kind = DriftKind::ClusterMissing;
        finding.subject = expected.value();
        finding.intended = "member of the intended federation";
        finding.observed = "not present in the observed federation";
        finding.basis = ReasonBasis::Derived;
        finding.precision = intended.precision;
        finding.evidence_class = intended.evidence_class;
        const Status s = finding.evidence.add(Provenance::ExternalInventory, intended.evidence_class,
                                              "intended-state", intended.precision,
                                              "intended state lists this cluster");
        (void)s;
        report.findings.push_back(std::move(finding));
        continue;
      }
      if (!is_current(cluster->currentness)) {
        DriftFinding finding;
        finding.kind = DriftKind::StaleCluster;
        finding.subject = expected.value();
        finding.intended = "current evidence";
        finding.observed = std::string(fo::to_string(cluster->currentness));
        finding.basis = ReasonBasis::Observed;
        finding.precision = Precision::Exact;
        finding.evidence_class = cluster->stamp.evidence_class;
        const Status s = finding.evidence.add(Provenance::ClusterController,
                                              cluster->stamp.evidence_class,
                                              cluster->stamp.publisher.value(),
                                              cluster->stamp.precision,
                                              "cluster currentness observed");
        (void)s;
        report.findings.push_back(std::move(finding));
      }
    }
    // Policy-generation drift is a property of the federation, not of one cluster, so it
    // is evaluated once against the federation record.
    if (intended.expected_policy_generation.is_set()) {
      const FederationRecord* record = snapshot.find_federation(report.federation);
      const PolicyGeneration observed =
          record != nullptr ? record->policy_generation : PolicyGeneration{};
      if (observed != intended.expected_policy_generation) {
        DriftFinding finding;
        finding.kind = DriftKind::PolicyGenerationMismatch;
        finding.subject = report.federation.value();
        finding.intended =
            "policy generation " + intended.expected_policy_generation.to_string();
        finding.observed = observed.is_set() ? "policy generation " + observed.to_string()
                                             : "no policy generation published";
        finding.basis = ReasonBasis::Derived;
        finding.precision = intended.precision;
        finding.evidence_class =
            record != nullptr ? record->stamp.evidence_class : intended.evidence_class;
        const Status s = finding.evidence.add(Provenance::ExternalInventory,
                                              intended.evidence_class, "intended-state",
                                              intended.precision,
                                              "intended policy generation differs from the "
                                              "observed policy generation");
        (void)s;
        report.findings.push_back(std::move(finding));
      }
    }
    for (const SiteId& expected : intended.expected_sites) {
      if (snapshot.find_site(expected) == nullptr) {
        DriftFinding finding;
        finding.kind = DriftKind::SiteMissing;
        finding.subject = expected.value();
        finding.intended = "member site of the intended federation";
        finding.observed = "not present in the observed federation";
        finding.basis = ReasonBasis::Derived;
        finding.precision = intended.precision;
        finding.evidence_class = intended.evidence_class;
        const Status s = finding.evidence.add(Provenance::ExternalInventory, intended.evidence_class,
                                              "intended-state", intended.precision,
                                              "intended state lists this site");
        (void)s;
        report.findings.push_back(std::move(finding));
      }
    }
    for (const ClusterRecord& cluster : snapshot.clusters) {
      if (std::find(intended.expected_clusters.begin(), intended.expected_clusters.end(),
                    cluster.id) == intended.expected_clusters.end() &&
          !intended.expected_clusters.empty()) {
        DriftFinding finding;
        finding.kind = DriftKind::UnexpectedCluster;
        finding.subject = cluster.id.value();
        finding.intended = "not listed in the intended federation";
        finding.observed = "present in the observed federation";
        finding.basis = ReasonBasis::Derived;
        finding.precision = intended.precision;
        finding.evidence_class = cluster.stamp.evidence_class;
        const Status s = finding.evidence.add(Provenance::ExternalInventory, intended.evidence_class,
                                              "intended-state", intended.precision,
                                              "intended state does not list this cluster");
        (void)s;
        report.findings.push_back(std::move(finding));
      }
    }
    for (const IntendedFederationState::ExpectedCapability& expected : intended.expected_capabilities) {
      const ClusterRecord* cluster = snapshot.find_cluster(expected.cluster);
      if (cluster == nullptr) {
        continue;
      }
      CapabilityRequirement requirement;
      requirement.key = expected.key;
      requirement.comparator = expected.comparator;
      requirement.value = expected.value;
      const CapabilityCheck check = evaluate_requirement(requirement, cluster->capabilities);
      if (check.satisfied == Tri::Yes) {
        continue;
      }
      DriftFinding finding;
      finding.kind = DriftKind::CapabilityMismatch;
      finding.subject = expected.cluster.value() + "/" + expected.key.to_string();
      finding.intended = requirement.to_string();
      finding.observed = check.observed.known() ? check.observed.to_string() : std::string("UNKNOWN");
      finding.basis = ReasonBasis::Derived;
      finding.precision = check.satisfied == Tri::Unknown ? Precision::Unknown : Precision::Derived;
      finding.evidence_class = cluster->stamp.evidence_class;
      const Status s = finding.evidence.add(Provenance::DerivedAnalysis,
                                            cluster->stamp.evidence_class, "drift-analysis",
                                            Precision::Derived, check.detail);
      (void)s;
      report.findings.push_back(std::move(finding));
    }
    for (const auto& expected : intended.expected_runtime_versions) {
      const ClusterRecord* cluster = snapshot.find_cluster(expected.first);
      if (cluster == nullptr) {
        continue;
      }
      const RuntimeRecord* runtime = primary_runtime(snapshot, *cluster);
      if (runtime == nullptr) {
        DriftFinding finding;
        finding.kind = DriftKind::RuntimeVersionDivergence;
        finding.subject = expected.first.value();
        finding.intended = expected.second;
        finding.observed = "no runtime published";
        finding.basis = ReasonBasis::Observed;
        finding.precision = Precision::Unknown;
        finding.evidence_class = cluster->stamp.evidence_class;
        report.findings.push_back(std::move(finding));
        continue;
      }
      if (runtime->version != expected.second) {
        DriftFinding finding;
        finding.kind = DriftKind::RuntimeVersionDivergence;
        finding.subject = expected.first.value() + "/" + runtime->id.value();
        finding.intended = expected.second;
        finding.observed = runtime->version;
        finding.basis = ReasonBasis::Derived;
        finding.precision = runtime->stamp.precision;
        finding.evidence_class = runtime->stamp.evidence_class;
        const Status s = finding.evidence.add(Provenance::RuntimeRegistry,
                                              runtime->stamp.evidence_class,
                                              runtime->stamp.publisher.value(),
                                              runtime->stamp.precision,
                                              "runtime version observed");
        (void)s;
        report.findings.push_back(std::move(finding));
      }
    }
    for (const auto& expected : intended.expected_capacity) {
      const ClusterRecord* cluster = snapshot.find_cluster(expected.first);
      if (cluster == nullptr) {
        continue;
      }
      const Result<CapacityLedger> observed = cluster->total_ledger(ResourceKind::Accelerator);
      if (!observed.ok()) {
        continue;
      }
      if (!(observed.value() == expected.second)) {
        DriftFinding finding;
        finding.kind = DriftKind::CapacityReportDivergence;
        finding.subject = expected.first.value();
        finding.intended = "nominal=" + std::to_string(expected.second.nominal) +
                           " idle=" + std::to_string(expected.second.idle);
        finding.observed = "nominal=" + std::to_string(observed.value().nominal) +
                           " idle=" + std::to_string(observed.value().idle);
        finding.basis = ReasonBasis::Derived;
        finding.precision = Precision::Derived;
        finding.evidence_class = cluster->stamp.evidence_class;
        report.findings.push_back(std::move(finding));
      }
    }
  }

  // Behavioral placement drift between two windows.
  if (request.behavior_window_supplied) {
    std::map<WorkloadId, const PlacementRecord*> before;
    std::map<WorkloadId, const PlacementRecord*> after;
    for (const PlacementRecord& placement : snapshot.placements) {
      if (!report.federation.empty() && placement.federation != report.federation) {
        continue;
      }
      if (request.before_window.contains(placement.stamp.observed_at)) {
        auto it = before.find(placement.workload);
        if (it == before.end() ||
            it->second->stamp.observed_at < placement.stamp.observed_at) {
          before[placement.workload] = &placement;
        }
      }
      if (request.after_window.contains(placement.stamp.observed_at)) {
        auto it = after.find(placement.workload);
        if (it == after.end() || it->second->stamp.observed_at < placement.stamp.observed_at) {
          after[placement.workload] = &placement;
        }
      }
    }
    for (const auto& entry : after) {
      const auto before_it = before.find(entry.first);
      if (before_it == before.end()) {
        continue;
      }
      const PlacementRecord& old_placement = *before_it->second;
      const PlacementRecord& new_placement = *entry.second;
      DriftFinding finding;
      finding.subject = entry.first.value();
      finding.basis = ReasonBasis::Derived;
      finding.precision = weakest(old_placement.stamp.precision, new_placement.stamp.precision);
      finding.evidence_class =
          weaker(old_placement.stamp.evidence_class, new_placement.stamp.evidence_class);
      bool emit = false;
      if (old_placement.selected != new_placement.selected) {
        finding.kind = DriftKind::PlacementTargetChanged;
        finding.intended = "selected " + old_placement.selected.value();
        finding.observed = "selected " + new_placement.selected.value();
        emit = true;
        const Status s = finding.evidence.add(
            Provenance::Scheduler, new_placement.stamp.evidence_class, "placement-history",
            Precision::Derived,
            "placement target changed between the two observation windows; a change is not by "
            "itself a regression");
        (void)s;
      }
      if (old_placement.compatibility_constrained != new_placement.compatibility_constrained) {
        finding.kind = DriftKind::PlacementCompatibilityChanged;
        finding.intended = std::string("compatibility_constrained=") +
                           std::string(fo::to_string(old_placement.compatibility_constrained));
        finding.observed = std::string("compatibility_constrained=") +
                           std::string(fo::to_string(new_placement.compatibility_constrained));
        emit = true;
      }
      if (!old_placement.fallback_required && new_placement.fallback_required) {
        finding.kind = DriftKind::PlacementFallbackIntroduced;
        finding.intended = "no fallback";
        finding.observed = new_placement.fallback_detail.empty() ? "fallback required"
                                                                 : new_placement.fallback_detail;
        emit = true;
      }
      // Rejection reason set differences.
      std::vector<std::string> old_reasons;
      std::vector<std::string> new_reasons;
      for (const CandidateObservation& candidate : old_placement.candidates) {
        for (const RejectionReason reason : candidate.reason_list()) {
          old_reasons.emplace_back(fo::to_string(reason));
        }
      }
      for (const CandidateObservation& candidate : new_placement.candidates) {
        for (const RejectionReason reason : candidate.reason_list()) {
          new_reasons.emplace_back(fo::to_string(reason));
        }
      }
      std::sort(old_reasons.begin(), old_reasons.end());
      std::sort(new_reasons.begin(), new_reasons.end());
      if (old_reasons != new_reasons) {
        const auto missing_in_new = [&] {
          std::vector<std::string> out;
          std::set_difference(new_reasons.begin(), new_reasons.end(), old_reasons.begin(),
                              old_reasons.end(), std::back_inserter(out));
          return out;
        }();
        const auto cleared = [&] {
          std::vector<std::string> out;
          std::set_difference(old_reasons.begin(), old_reasons.end(), new_reasons.begin(),
                              new_reasons.end(), std::back_inserter(out));
          return out;
        }();
        if (!missing_in_new.empty()) {
          DriftFinding rejection;
          rejection.kind = DriftKind::PlacementRejectionIntroduced;
          rejection.subject = entry.first.value();
          rejection.intended = "no new rejection reasons";
          rejection.observed = join_strings(missing_in_new, ",");
          rejection.basis = ReasonBasis::Derived;
          rejection.precision = Precision::Derived;
          rejection.evidence_class = finding.evidence_class;
          report.findings.push_back(std::move(rejection));
        }
        if (!cleared.empty()) {
          DriftFinding rejection;
          rejection.kind = DriftKind::PlacementRejectionCleared;
          rejection.subject = entry.first.value();
          rejection.intended = "previous rejection reasons";
          rejection.observed = "cleared: " + join_strings(cleared, ",");
          rejection.basis = ReasonBasis::Derived;
          rejection.precision = Precision::Derived;
          rejection.evidence_class = finding.evidence_class;
          report.findings.push_back(std::move(rejection));
        }
      }
      if (emit) {
        report.findings.push_back(std::move(finding));
      }
    }
  }

  std::sort(report.findings.begin(), report.findings.end());

  Precision precision = Precision::Derived;
  EvidenceClass evidence_class = snapshot.evidence_class;
  bool first = true;
  for (const DriftFinding& finding : report.findings) {
    precision = first ? finding.precision : weakest(precision, finding.precision);
    evidence_class = first ? finding.evidence_class : weaker(evidence_class, finding.evidence_class);
    first = false;
  }
  report.precision = first ? Precision::Unknown : precision;
  report.evidence_class = evidence_class;

  const Status s = report.evidence.add(
      Provenance::DerivedAnalysis, evidence_class, "drift-analysis", Precision::Derived,
      std::string("intended_state_supplied=") + (report.intended_state_supplied ? "yes" : "no") +
          " behavior_windows_supplied=" + (report.behavior_window_supplied ? "yes" : "no") +
          " findings=" + std::to_string(report.findings.size()));
  (void)s;
  return report;
}

}  // namespace fo
