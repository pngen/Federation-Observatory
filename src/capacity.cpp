// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Exact capacity accounting. The ledger decomposition is closed by construction and
// re-verified on every validation; nothing in the runtime is allowed to add an extra
// bucket to it.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "federation_observatory/capacity.hpp"
#include "federation_observatory/explanation.hpp"

namespace fo {

std::string_view to_string(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Accelerator: return "ACCELERATOR";
    case ResourceKind::MemoryBytes: return "MEMORY_BYTES";
    case ResourceKind::Custom: return "CUSTOM";
  }
  return "CUSTOM";
}

bool parse_resource_kind(std::string_view text, ResourceKind& out) noexcept {
  for (int i = 0; i <= static_cast<int>(ResourceKind::Custom); ++i) {
    const auto candidate = static_cast<ResourceKind>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

Result<CapacityLedger> CapacityLedger::from_components(std::uint64_t nominal, std::uint64_t offline,
                                                       std::uint64_t allocated, std::uint64_t reserved,
                                                       std::uint64_t draining, std::uint64_t unusable) {
  std::uint64_t committed_total = 0;
  if (!checked_sum(&offline, 1, committed_total)) {
    return Error(ErrorCode::CapacityInconsistent, "capacity accounting overflowed");
  }
  const std::uint64_t components[] = {allocated, reserved, draining, unusable};
  for (const std::uint64_t component : components) {
    if (!checked_add(committed_total, component, committed_total)) {
      return Error(ErrorCode::CapacityInconsistent, "capacity accounting overflowed");
    }
  }
  std::uint64_t idle = 0;
  if (!checked_sub(nominal, committed_total, idle)) {
    return Error(ErrorCode::CapacityInconsistent,
                 "capacity components exceed nominal capacity",
                 "nominal=" + std::to_string(nominal) +
                     " components=" + std::to_string(committed_total));
  }
  CapacityLedger ledger;
  ledger.nominal = nominal;
  ledger.offline = offline;
  ledger.allocated = allocated;
  ledger.reserved = reserved;
  ledger.draining = draining;
  ledger.unusable = unusable;
  ledger.idle = idle;
  return ledger;
}

std::uint64_t CapacityLedger::online() const noexcept {
  std::uint64_t value = 0;
  return checked_sub(nominal, offline, value) ? value : 0;
}

std::uint64_t CapacityLedger::committed() const noexcept {
  std::uint64_t total = 0;
  if (!checked_add(allocated, reserved, total)) {
    return UINT64_MAX;
  }
  if (!checked_add(total, draining, total)) {
    return UINT64_MAX;
  }
  return total;
}

Status CapacityLedger::validate() const {
  std::uint64_t expected_idle = 0;
  const Result<CapacityLedger> rebuilt =
      CapacityLedger::from_components(nominal, offline, allocated, reserved, draining, unusable);
  if (!rebuilt.ok()) {
    return rebuilt.error();
  }
  expected_idle = rebuilt.value().idle;
  if (expected_idle != idle) {
    return fail(ErrorCode::CapacityInconsistent, "capacity ledger does not close",
                "idle=" + std::to_string(idle) + " expected=" + std::to_string(expected_idle));
  }
  return Status::success();
}

bool operator==(const CapacityLedger& a, const CapacityLedger& b) noexcept {
  return a.nominal == b.nominal && a.offline == b.offline && a.allocated == b.allocated &&
         a.reserved == b.reserved && a.draining == b.draining && a.unusable == b.unusable &&
         a.idle == b.idle;
}

std::string CapacityLedger::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"nominal", std::to_string(nominal)});
  rows.push_back({"offline", std::to_string(offline)});
  rows.push_back({"online", std::to_string(online())});
  rows.push_back({"allocated", std::to_string(allocated)});
  rows.push_back({"reserved", std::to_string(reserved)});
  rows.push_back({"draining", std::to_string(draining)});
  rows.push_back({"unusable", std::to_string(unusable)});
  rows.push_back({"idle", std::to_string(idle)});
  return render_table({"capacity", "value"}, rows, indent);
}

bool operator==(const CapacityPool& a, const CapacityPool& b) noexcept {
  return a.pool_id == b.pool_id && a.kind == b.kind && a.accelerator_class == b.accelerator_class &&
         a.custom_name == b.custom_name && a.ledger == b.ledger && a.generation == b.generation &&
         a.precision == b.precision && a.evidence_class == b.evidence_class;
}

bool operator<(const CapacityPool& a, const CapacityPool& b) noexcept {
  if (a.pool_id != b.pool_id) {
    return a.pool_id < b.pool_id;
  }
  if (a.kind != b.kind) {
    return a.kind < b.kind;
  }
  return a.accelerator_class < b.accelerator_class;
}

std::string CapacityPool::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"pool", pool_id.value()});
  rows.push_back({"kind", std::string(fo::to_string(kind))});
  rows.push_back({"accelerator_class", render_id(accelerator_class.value())});
  rows.push_back({"custom_name", custom_name.empty() ? "-" : custom_name});
  rows.push_back({"capacity_generation", generation.is_set() ? generation.to_string() : "-"});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = std::string(indent) + "capacity_pool:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  out += '\n';
  out += ledger.render(std::string(indent) + "  ");
  return out;
}

std::string_view to_string(StrandingReason reason) noexcept {
  switch (reason) {
    case StrandingReason::None: return "NONE";
    case StrandingReason::UnsupportedAcceleratorCapability: return "UNSUPPORTED_ACCELERATOR_CAPABILITY";
    case StrandingReason::UnsupportedPrecision: return "UNSUPPORTED_PRECISION";
    case StrandingReason::MissingOffloadCapability: return "MISSING_OFFLOAD_CAPABILITY";
    case StrandingReason::MissingInterconnectFeature: return "MISSING_INTERCONNECT_FEATURE";
    case StrandingReason::RuntimeMismatch: return "RUNTIME_MISMATCH";
    case StrandingReason::DriverMismatch: return "DRIVER_MISMATCH";
    case StrandingReason::AbiMismatch: return "ABI_MISMATCH";
    case StrandingReason::ArtifactIncompatibility: return "ARTIFACT_INCOMPATIBILITY";
    case StrandingReason::MissingCompilerTarget: return "MISSING_COMPILER_TARGET";
    case StrandingReason::PartitionGeometry: return "PARTITION_GEOMETRY";
    case StrandingReason::TopologyConstraint: return "TOPOLOGY_CONSTRAINT";
    case StrandingReason::IsolationRequirement: return "ISOLATION_REQUIREMENT";
    case StrandingReason::MissingLocalStorageState: return "MISSING_LOCAL_STORAGE_STATE";
    case StrandingReason::MemoryInsufficient: return "MEMORY_INSUFFICIENT";
    case StrandingReason::SiteRestriction: return "SITE_RESTRICTION";
    case StrandingReason::PolicyRestriction: return "POLICY_RESTRICTION";
    case StrandingReason::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool parse_stranding_reason(std::string_view text, StrandingReason& out) noexcept {
  for (std::size_t i = 0; i < kStrandingReasonCount; ++i) {
    const auto candidate = static_cast<StrandingReason>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::size_t stranding_reason_precedence(StrandingReason reason) noexcept {
  switch (reason) {
    case StrandingReason::Unknown: return 0;
    case StrandingReason::UnsupportedAcceleratorCapability: return 1;
    case StrandingReason::UnsupportedPrecision: return 2;
    case StrandingReason::MissingOffloadCapability: return 3;
    case StrandingReason::MissingInterconnectFeature: return 4;
    case StrandingReason::RuntimeMismatch: return 5;
    case StrandingReason::DriverMismatch: return 6;
    case StrandingReason::AbiMismatch: return 7;
    case StrandingReason::ArtifactIncompatibility: return 8;
    case StrandingReason::MissingCompilerTarget: return 9;
    case StrandingReason::PartitionGeometry: return 10;
    case StrandingReason::TopologyConstraint: return 11;
    case StrandingReason::IsolationRequirement: return 12;
    case StrandingReason::MissingLocalStorageState: return 13;
    case StrandingReason::MemoryInsufficient: return 14;
    case StrandingReason::SiteRestriction: return 15;
    case StrandingReason::PolicyRestriction: return 16;
    case StrandingReason::None: return 17;
  }
  return 0;
}

bool is_technical_reason(StrandingReason reason) noexcept {
  switch (reason) {
    case StrandingReason::SiteRestriction:
    case StrandingReason::PolicyRestriction:
    case StrandingReason::None:
    case StrandingReason::Unknown:
      return false;
    default:
      return true;
  }
}

Status ReasonTally::add(StrandingReason reason, std::uint64_t amount) noexcept {
  const auto index = static_cast<std::size_t>(reason);
  if (index >= kStrandingReasonCount) {
    return fail(ErrorCode::InvalidArgument, "stranding reason out of range");
  }
  std::uint64_t updated = 0;
  if (!checked_add(amounts_[index], amount, updated)) {
    return fail(ErrorCode::CapacityInconsistent, "reason tally overflowed",
                std::string(fo::to_string(reason)));
  }
  amounts_[index] = updated;
  return Status::success();
}

std::uint64_t ReasonTally::get(StrandingReason reason) const noexcept {
  const auto index = static_cast<std::size_t>(reason);
  return index < kStrandingReasonCount ? amounts_[index] : 0;
}

std::uint64_t ReasonTally::total() const noexcept {
  std::uint64_t out = 0;
  return checked_sum(amounts_.data(), amounts_.size(), out) ? out : UINT64_MAX;
}

std::vector<std::pair<StrandingReason, std::uint64_t>> ReasonTally::non_zero() const {
  std::vector<std::pair<StrandingReason, std::uint64_t>> out;
  for (std::size_t i = 0; i < kStrandingReasonCount; ++i) {
    if (amounts_[i] != 0) {
      out.emplace_back(static_cast<StrandingReason>(i), amounts_[i]);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return stranding_reason_precedence(a.first) < stranding_reason_precedence(b.first);
  });
  return out;
}

std::uint64_t EligibilityBreakdown::stranded() const noexcept {
  std::uint64_t out = 0;
  return checked_sub(idle, usable, out) ? (checked_sub(out, unknown, out) ? out : 0) : 0;
}

bool EligibilityBreakdown::closes() const noexcept {
  std::uint64_t expected = 0;
  if (!checked_add(usable, unknown, expected)) {
    return false;
  }
  if (!checked_add(expected, stranded_by_primary_reason.total(), expected)) {
    return false;
  }
  return expected == idle;
}

Status EligibilityBreakdown::validate() const {
  std::uint64_t expected = 0;
  if (!checked_add(usable, unknown, expected)) {
    return fail(ErrorCode::CapacityInconsistent, "eligibility breakdown overflowed");
  }
  if (!checked_add(expected, stranded_by_primary_reason.total(), expected)) {
    return fail(ErrorCode::CapacityInconsistent, "eligibility breakdown overflowed");
  }
  if (expected != idle) {
    return fail(ErrorCode::CapacityInconsistent, "eligibility breakdown does not close",
                "idle=" + std::to_string(idle) + " accounted=" + std::to_string(expected));
  }
  return Status::success();
}

std::string EligibilityBreakdown::stranded_percent_of_idle() const {
  return percent_string(stranded(), idle);
}

void EligibilityBreakdown::merge(const EligibilityBreakdown& other) {
  std::uint64_t value = 0;
  if (checked_add(idle, other.idle, value)) idle = value;
  if (checked_add(usable, other.usable, value)) usable = value;
  if (checked_add(unknown, other.unknown, value)) unknown = value;
  for (std::size_t i = 0; i < kStrandingReasonCount; ++i) {
    const auto reason = static_cast<StrandingReason>(i);
    const std::uint64_t a = stranded_by_primary_reason.get(reason);
    const std::uint64_t b = other.stranded_by_primary_reason.get(reason);
    if (checked_add(a, b, value)) {
      const Status s = stranded_by_primary_reason.add(reason, value - a);
      (void)s;
    }
    const std::uint64_t oa = reason_occurrences.get(reason);
    const std::uint64_t ob = other.reason_occurrences.get(reason);
    if (checked_add(oa, ob, value)) {
      const Status s = reason_occurrences.add(reason, value - oa);
      (void)s;
    }
  }
}

std::string EligibilityBreakdown::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"idle", std::to_string(idle)});
  rows.push_back({"usable", std::to_string(usable)});
  rows.push_back({"unknown", std::to_string(unknown)});
  rows.push_back({"stranded", std::to_string(stranded())});
  rows.push_back({"stranded_percent_of_idle", stranded_percent_of_idle()});
  std::string out = render_table({"eligibility", "value"}, rows, indent);
  const auto reasons = stranded_by_primary_reason.non_zero();
  if (!reasons.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> reason_rows;
    for (const auto& entry : reasons) {
      reason_rows.push_back({std::string(fo::to_string(entry.first)), std::to_string(entry.second),
                             is_technical_reason(entry.first) ? "technical" : "policy"});
    }
    out += render_table({"primary_stranding_reason", "amount", "nature"}, reason_rows,
                        std::string(indent) + "  ");
  }
  const auto occurrences = reason_occurrences.non_zero();
  if (!occurrences.empty()) {
    out += '\n';
    std::vector<std::vector<std::string>> occurrence_rows;
    for (const auto& entry : occurrences) {
      occurrence_rows.push_back(
          {std::string(fo::to_string(entry.first)), std::to_string(entry.second)});
    }
    out += render_table({"reason_occurrence", "amount"}, occurrence_rows,
                        std::string(indent) + "  ");
    out += '\n';
    out += std::string(indent) +
           "note: reason_occurrence tallies overlap; a capacity unit that fails several\n";
    out += std::string(indent) +
           "      requirements is counted once in each occurrence tally and therefore\n";
    out += std::string(indent) +
           "      deliberately does not sum to the stranded total.";
  }
  return out;
}

std::string CapacitySummary::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"nominal", std::to_string(nominal)});
  rows.push_back({"offline", std::to_string(offline)});
  rows.push_back({"allocated", std::to_string(allocated)});
  rows.push_back({"reserved", std::to_string(reserved)});
  rows.push_back({"draining", std::to_string(draining)});
  rows.push_back({"unusable", std::to_string(unusable)});
  rows.push_back({"idle", std::to_string(idle)});
  rows.push_back({"usable", std::to_string(usable)});
  rows.push_back({"stranded", std::to_string(stranded)});
  rows.push_back({"unknown_eligibility", std::to_string(unknown)});
  rows.push_back({"stranded_percent_of_nominal", percent_string(stranded, nominal)});
  rows.push_back({"stranded_percent_of_idle", percent_string(stranded, idle)});
  rows.push_back({"usable_percent_of_nominal", percent_string(usable, nominal)});
  return render_table({"capacity", "value"}, rows, indent);
}

}  // namespace fo
