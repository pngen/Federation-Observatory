// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Structured capability model: typed keys, bounded typed values, tri-state requirement
// evaluation. Absence of evidence never becomes evidence of absence.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/capability.hpp"
#include "federation_observatory/explanation.hpp"

namespace fo {
namespace {

std::string format_double(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  const bool negative = value < 0;
  const double magnitude = negative ? -value : value;
  const double integral = std::floor(magnitude);
  if (magnitude == integral && magnitude < 1.0e15) {
    const std::string text = std::to_string(static_cast<std::uint64_t>(integral));
    return negative ? "-" + text : text;
  }
  const std::uint64_t whole = static_cast<std::uint64_t>(integral);
  std::uint64_t fraction = static_cast<std::uint64_t>((magnitude - integral) * 1000000.0 + 0.5);
  if (fraction >= 1000000ull) {
    fraction = 999999ull;
  }
  std::string frac = std::to_string(fraction);
  frac.insert(0, 6 - frac.size(), '0');
  std::string text = std::to_string(whole) + "." + frac;
  return negative ? "-" + text : text;
}

/// Ordering result plus whether the two values were comparable at all.
struct Comparison {
  bool comparable = false;
  int order = 0;
};

Comparison compare_numeric_pairs(const CapabilityValue& a, const CapabilityValue& b) {
  if (a.is_integral() && b.is_integral()) {
    const bool a_unsigned = a.as_integer(0) >= 0;
    if (a_unsigned && b.as_integer(0) >= 0) {
      const std::uint64_t av = a.as_unsigned(0);
      const std::uint64_t bv = b.as_unsigned(0);
      return Comparison{true, av < bv ? -1 : (av > bv ? 1 : 0)};
    }
    const std::int64_t av = a.as_integer(0);
    const std::int64_t bv = b.as_integer(0);
    return Comparison{true, av < bv ? -1 : (av > bv ? 1 : 0)};
  }
  if (a.is_integral() || a.is_number()) {
    if (b.is_integral() || b.is_number()) {
      const double av = a.is_number() ? a.as_number(0.0) : static_cast<double>(a.as_integer(0));
      const double bv = b.is_number() ? b.as_number(0.0) : static_cast<double>(b.as_integer(0));
      return Comparison{true, av < bv ? -1 : (av > bv ? 1 : 0)};
    }
  }
  return Comparison{};
}

bool text_set_contains(const CapabilityValue::TextSet& set, std::string_view needle) {
  return std::find(set.begin(), set.end(), needle) != set.end();
}

}  // namespace

std::string_view to_string(CapabilityCategory category) noexcept {
  switch (category) {
    case CapabilityCategory::AcceleratorArchitecture: return "ACCELERATOR_ARCHITECTURE";
    case CapabilityCategory::PrecisionSupport: return "PRECISION_SUPPORT";
    case CapabilityCategory::MemorySize: return "MEMORY_SIZE";
    case CapabilityCategory::MemoryBandwidthClass: return "MEMORY_BANDWIDTH_CLASS";
    case CapabilityCategory::Interconnect: return "INTERCONNECT";
    case CapabilityCategory::PartitionSupport: return "PARTITION_SUPPORT";
    case CapabilityCategory::RuntimeApi: return "RUNTIME_API";
    case CapabilityCategory::DriverApi: return "DRIVER_API";
    case CapabilityCategory::CompilerTarget: return "COMPILER_TARGET";
    case CapabilityCategory::KernelFormat: return "KERNEL_FORMAT";
    case CapabilityCategory::CollectiveSupport: return "COLLECTIVE_SUPPORT";
    case CapabilityCategory::RdmaCapability: return "RDMA_CAPABILITY";
    case CapabilityCategory::GpuDirectCapability: return "GPUDIRECT_CAPABILITY";
    case CapabilityCategory::CxlCapability: return "CXL_CAPABILITY";
    case CapabilityCategory::DpuSmartNicCapability: return "DPU_SMARTNIC_CAPABILITY";
    case CapabilityCategory::ArtifactFormat: return "ARTIFACT_FORMAT";
    case CapabilityCategory::QuantizationSupport: return "QUANTIZATION_SUPPORT";
    case CapabilityCategory::SparseExecutionSupport: return "SPARSE_EXECUTION_SUPPORT";
    case CapabilityCategory::FirmwareGeneration: return "FIRMWARE_GENERATION";
    case CapabilityCategory::ContainerRuntime: return "CONTAINER_RUNTIME";
    case CapabilityCategory::OperatingSystem: return "OPERATING_SYSTEM";
    case CapabilityCategory::SecurityIsolation: return "SECURITY_ISOLATION";
    case CapabilityCategory::OffloadCapability: return "OFFLOAD_CAPABILITY";
    case CapabilityCategory::Custom: return "CUSTOM";
  }
  return "CUSTOM";
}

bool parse_capability_category(std::string_view text, CapabilityCategory& out) noexcept {
  for (int i = 0; i <= static_cast<int>(CapabilityCategory::Custom); ++i) {
    const auto candidate = static_cast<CapabilityCategory>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

namespace {

struct KeyEntry {
  CapabilityKey key;
  const char* name;
  CapabilityCategory category;
};

constexpr KeyEntry kKeyTable[] = {
    {CapabilityKey::AcceleratorArchitecture, "accelerator.architecture",
     CapabilityCategory::AcceleratorArchitecture},
    {CapabilityKey::AcceleratorVendor, "accelerator.vendor", CapabilityCategory::AcceleratorArchitecture},
    {CapabilityKey::AcceleratorFamily, "accelerator.family", CapabilityCategory::AcceleratorArchitecture},
    {CapabilityKey::AcceleratorModel, "accelerator.model", CapabilityCategory::AcceleratorArchitecture},
    {CapabilityKey::ComputeCapability, "accelerator.compute_capability",
     CapabilityCategory::AcceleratorArchitecture},
    {CapabilityKey::PrecisionFp64, "precision.fp64", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp32, "precision.fp32", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionTf32, "precision.tf32", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp16, "precision.fp16", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionBf16, "precision.bf16", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp8E4M3, "precision.fp8_e4m3", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp8E5M2, "precision.fp8_e5m2", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp6, "precision.fp6", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionInt8, "precision.int8", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionInt4, "precision.int4", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::PrecisionFp4, "precision.fp4", CapabilityCategory::PrecisionSupport},
    {CapabilityKey::MemoryBytes, "memory.bytes", CapabilityCategory::MemorySize},
    {CapabilityKey::MemoryBandwidthGbps, "memory.bandwidth_gbps",
     CapabilityCategory::MemoryBandwidthClass},
    {CapabilityKey::MemoryType, "memory.type", CapabilityCategory::MemorySize},
    {CapabilityKey::MemoryEcc, "memory.ecc", CapabilityCategory::MemorySize},
    {CapabilityKey::InterconnectType, "interconnect.type", CapabilityCategory::Interconnect},
    {CapabilityKey::InterconnectBandwidthGbps, "interconnect.bandwidth_gbps",
     CapabilityCategory::Interconnect},
    {CapabilityKey::NvlinkGeneration, "interconnect.nvlink_generation",
     CapabilityCategory::Interconnect},
    {CapabilityKey::PcieGeneration, "interconnect.pcie_generation", CapabilityCategory::Interconnect},
    {CapabilityKey::PartitionSupport, "partition.support", CapabilityCategory::PartitionSupport},
    {CapabilityKey::PartitionModes, "partition.modes", CapabilityCategory::PartitionSupport},
    {CapabilityKey::PartitionGeometry, "partition.geometry", CapabilityCategory::PartitionSupport},
    {CapabilityKey::PartitionMaxInstances, "partition.max_instances",
     CapabilityCategory::PartitionSupport},
    {CapabilityKey::RuntimeApi, "runtime.api", CapabilityCategory::RuntimeApi},
    {CapabilityKey::RuntimeVersion, "runtime.version", CapabilityCategory::RuntimeApi},
    {CapabilityKey::RuntimeAbi, "runtime.abi", CapabilityCategory::RuntimeApi},
    {CapabilityKey::RuntimeGeneration, "runtime.generation", CapabilityCategory::RuntimeApi},
    {CapabilityKey::DriverApi, "driver.api", CapabilityCategory::DriverApi},
    {CapabilityKey::DriverVersion, "driver.version", CapabilityCategory::DriverApi},
    {CapabilityKey::DriverAbi, "driver.abi", CapabilityCategory::DriverApi},
    {CapabilityKey::DriverGeneration, "driver.generation", CapabilityCategory::DriverApi},
    {CapabilityKey::CompilerTarget, "compiler.target", CapabilityCategory::CompilerTarget},
    {CapabilityKey::CompilerVersion, "compiler.version", CapabilityCategory::CompilerTarget},
    {CapabilityKey::KernelFormat, "kernel.format", CapabilityCategory::KernelFormat},
    {CapabilityKey::KernelArchitectures, "kernel.architectures", CapabilityCategory::KernelFormat},
    {CapabilityKey::CollectiveSupport, "collective.support", CapabilityCategory::CollectiveSupport},
    {CapabilityKey::CollectiveBackend, "collective.backend", CapabilityCategory::CollectiveSupport},
    {CapabilityKey::CollectiveMaxGroupSize, "collective.max_group_size",
     CapabilityCategory::CollectiveSupport},
    {CapabilityKey::RdmaCapability, "fabric.rdma", CapabilityCategory::RdmaCapability},
    {CapabilityKey::GpuDirectCapability, "fabric.gpu_direct", CapabilityCategory::GpuDirectCapability},
    {CapabilityKey::GpuDirectStorage, "fabric.gpu_direct_storage",
     CapabilityCategory::GpuDirectCapability},
    {CapabilityKey::CxlCapability, "fabric.cxl", CapabilityCategory::CxlCapability},
    {CapabilityKey::DpuSmartNicCapability, "fabric.dpu_smartnic",
     CapabilityCategory::DpuSmartNicCapability},
    {CapabilityKey::ArtifactFormat, "artifact.format", CapabilityCategory::ArtifactFormat},
    {CapabilityKey::ArtifactFormats, "artifact.formats", CapabilityCategory::ArtifactFormat},
    {CapabilityKey::QuantizationSupport, "quantization.support",
     CapabilityCategory::QuantizationSupport},
    {CapabilityKey::QuantizationFormats, "quantization.formats",
     CapabilityCategory::QuantizationSupport},
    {CapabilityKey::SparseExecutionSupport, "sparse.execution",
     CapabilityCategory::SparseExecutionSupport},
    {CapabilityKey::InferenceServingBackend, "serving.backend", CapabilityCategory::ArtifactFormat},
    {CapabilityKey::FirmwareGeneration, "firmware.generation",
     CapabilityCategory::FirmwareGeneration},
    {CapabilityKey::ContainerRuntime, "environment.container_runtime",
     CapabilityCategory::ContainerRuntime},
    {CapabilityKey::OperatingSystem, "environment.os", CapabilityCategory::OperatingSystem},
    {CapabilityKey::SecurityIsolation, "environment.isolation",
     CapabilityCategory::SecurityIsolation},
    {CapabilityKey::OffloadCapability, "offload.capability", CapabilityCategory::OffloadCapability},
};

}  // namespace

std::string_view to_string(CapabilityKey key) noexcept {
  if (key == CapabilityKey::Custom) {
    return "custom";
  }
  for (const KeyEntry& entry : kKeyTable) {
    if (entry.key == key) {
      return entry.name;
    }
  }
  return "unknown";
}

CapabilityCategory category_of(CapabilityKey key) noexcept {
  for (const KeyEntry& entry : kKeyTable) {
    if (entry.key == key) {
      return entry.category;
    }
  }
  return CapabilityCategory::Custom;
}

bool parse_capability_key(std::string_view text, CapabilityKey& out) noexcept {
  for (const KeyEntry& entry : kKeyTable) {
    if (text == entry.name) {
      out = entry.key;
      return true;
    }
  }
  if (text == "custom") {
    out = CapabilityKey::Custom;
    return true;
  }
  return false;
}

bool CapabilityRef::valid() const noexcept {
  if (key == CapabilityKey::Unknown) {
    return false;
  }
  if (key == CapabilityKey::Custom) {
    return !custom.empty();
  }
  return custom.empty();
}

CapabilityCategory CapabilityRef::category() const noexcept { return category_of(key); }

std::string CapabilityRef::to_string() const {
  if (key == CapabilityKey::Custom) {
    return std::string("custom:") + custom.value();
  }
  return std::string(fo::to_string(key));
}

bool operator==(const CapabilityRef& a, const CapabilityRef& b) noexcept {
  return a.key == b.key && a.custom == b.custom;
}

bool operator<(const CapabilityRef& a, const CapabilityRef& b) noexcept {
  if (a.key != b.key) {
    return static_cast<std::uint16_t>(a.key) < static_cast<std::uint16_t>(b.key);
  }
  return a.custom < b.custom;
}

CapabilityValue CapabilityValue::boolean(bool v) {
  CapabilityValue value;
  value.storage_ = v;
  return value;
}

CapabilityValue CapabilityValue::integer(std::int64_t v) {
  CapabilityValue value;
  value.storage_ = v;
  return value;
}

CapabilityValue CapabilityValue::unsigned_integer(std::uint64_t v) {
  CapabilityValue value;
  value.storage_ = v;
  return value;
}

CapabilityValue CapabilityValue::number(double v) {
  CapabilityValue value;
  value.storage_ = v;
  return value;
}

CapabilityValue CapabilityValue::text(std::string v) {
  CapabilityValue value;
  if (v.size() > kMaxStringFieldBytes) {
    v = truncate_with_marker(v, kMaxStringFieldBytes);
  }
  value.storage_ = std::move(v);
  return value;
}

Result<CapabilityValue> CapabilityValue::text_set(TextSet values) {
  if (values.size() > kMaxValueSetEntries) {
    return Error(ErrorCode::BoundExceeded, "capability value set exceeds the maximum size",
                 std::to_string(values.size()));
  }
  for (std::string& entry : values) {
    if (entry.size() > 256) {
      return Error(ErrorCode::BoundExceeded, "capability value set entry exceeds the maximum length");
    }
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  CapabilityValue value;
  value.storage_ = std::move(values);
  return value;
}

bool CapabilityValue::as_boolean(bool fallback) const noexcept {
  const auto* v = std::get_if<bool>(&storage_);
  return v != nullptr ? *v : fallback;
}

std::int64_t CapabilityValue::as_integer(std::int64_t fallback) const noexcept {
  if (const auto* v = std::get_if<std::int64_t>(&storage_)) {
    return *v;
  }
  if (const auto* u = std::get_if<std::uint64_t>(&storage_)) {
    return *u > static_cast<std::uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<std::int64_t>(*u);
  }
  return fallback;
}

std::uint64_t CapabilityValue::as_unsigned(std::uint64_t fallback) const noexcept {
  if (const auto* u = std::get_if<std::uint64_t>(&storage_)) {
    return *u;
  }
  if (const auto* v = std::get_if<std::int64_t>(&storage_)) {
    return *v < 0 ? fallback : static_cast<std::uint64_t>(*v);
  }
  return fallback;
}

double CapabilityValue::as_number(double fallback) const noexcept {
  if (const auto* d = std::get_if<double>(&storage_)) {
    return *d;
  }
  if (const auto* v = std::get_if<std::int64_t>(&storage_)) {
    return static_cast<double>(*v);
  }
  if (const auto* u = std::get_if<std::uint64_t>(&storage_)) {
    return static_cast<double>(*u);
  }
  return fallback;
}

std::string_view CapabilityValue::as_text() const noexcept {
  const auto* v = std::get_if<std::string>(&storage_);
  return v != nullptr ? std::string_view(*v) : std::string_view();
}

const CapabilityValue::TextSet& CapabilityValue::as_text_set() const noexcept {
  static const TextSet kEmpty{};
  const auto* v = std::get_if<TextSet>(&storage_);
  return v != nullptr ? *v : kEmpty;
}

std::string CapabilityValue::to_string() const {
  switch (storage_.index()) {
    case 0: return "UNKNOWN";
    case 1: return std::get<bool>(storage_) ? "true" : "false";
    case 2: return std::to_string(std::get<std::int64_t>(storage_));
    case 3: return std::to_string(std::get<std::uint64_t>(storage_));
    case 4: return format_double(std::get<double>(storage_));
    case 5: return std::get<std::string>(storage_);
    case 6: {
      const TextSet& set = std::get<TextSet>(storage_);
      std::string out = "[";
      for (std::size_t i = 0; i < set.size(); ++i) {
        if (i != 0) {
          out += ", ";
        }
        out += set[i];
      }
      out += ']';
      return out;
    }
    default: return "UNKNOWN";
  }
}

bool operator==(const CapabilityValue& a, const CapabilityValue& b) noexcept {
  if (a.storage_.index() != b.storage_.index()) {
    return false;
  }
  return a.storage_ == b.storage_;
}

bool operator<(const CapabilityValue& a, const CapabilityValue& b) noexcept {
  if (a.storage_.index() != b.storage_.index()) {
    return a.storage_.index() < b.storage_.index();
  }
  return a.storage_ < b.storage_;
}

bool operator==(const CapabilityEntry& a, const CapabilityEntry& b) noexcept {
  return a.key == b.key && a.value == b.value && a.precision == b.precision &&
         a.evidence_class == b.evidence_class && a.generation == b.generation && a.note == b.note;
}

bool operator<(const CapabilityEntry& a, const CapabilityEntry& b) noexcept {
  if (a.key != b.key) {
    return a.key < b.key;
  }
  if (!(a.value == b.value)) {
    return a.value < b.value;
  }
  return a.generation < b.generation;
}

Status CapabilitySet::put(CapabilityEntry entry) {
  if (!entry.key.valid()) {
    return fail(ErrorCode::InvalidArgument, "capability key is not valid", entry.key.to_string());
  }
  if (entry.note.size() > 512) {
    entry.note = truncate_with_marker(entry.note, 512);
  }
  const auto existing = std::find_if(entries_.begin(), entries_.end(),
                                     [&entry](const CapabilityEntry& e) { return e.key == entry.key; });
  if (existing != entries_.end()) {
    *existing = std::move(entry);
    std::stable_sort(entries_.begin(), entries_.end());
    return Status::success();
  }
  if (entries_.size() >= limit_) {
    return fail(ErrorCode::BoundExceeded, "capability set is full",
                std::to_string(limit_) + " entries retained");
  }
  entries_.push_back(std::move(entry));
  std::stable_sort(entries_.begin(), entries_.end());
  return Status::success();
}

const CapabilityEntry* CapabilitySet::find(const CapabilityRef& key) const noexcept {
  for (const CapabilityEntry& entry : entries_) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

Precision CapabilitySet::combined_precision() const noexcept {
  Precision result = Precision::Unknown;
  bool first = true;
  for (const CapabilityEntry& entry : entries_) {
    result = first ? entry.precision : weakest(result, entry.precision);
    first = false;
  }
  return result;
}

EvidenceClass CapabilitySet::combined_class() const noexcept {
  EvidenceClass result = EvidenceClass::Unknown;
  bool first = true;
  for (const CapabilityEntry& entry : entries_) {
    result = first ? entry.evidence_class : weaker(result, entry.evidence_class);
    first = false;
  }
  return result;
}

std::string CapabilitySet::render(std::string_view indent) const {
  if (entries_.empty()) {
    return std::string(indent) + "capabilities: <none published>";
  }
  std::vector<std::vector<std::string>> rows;
  rows.reserve(entries_.size());
  for (const CapabilityEntry& entry : entries_) {
    rows.push_back({entry.key.to_string(), entry.value.to_string(),
                    std::string(fo::to_string(entry.precision)),
                    std::string(fo::to_string(entry.evidence_class)),
                    entry.generation.is_set() ? entry.generation.to_string() : std::string("-")});
  }
  return std::string(indent) + "capabilities:\n" +
         render_table({"key", "value", "precision", "class", "generation"}, rows,
                      std::string(indent) + "  ");
}

std::string_view to_string(CapabilityComparator comparator) noexcept {
  switch (comparator) {
    case CapabilityComparator::Present: return "PRESENT";
    case CapabilityComparator::Absent: return "ABSENT";
    case CapabilityComparator::Equals: return "EQUALS";
    case CapabilityComparator::NotEquals: return "NOT_EQUALS";
    case CapabilityComparator::AtLeast: return "AT_LEAST";
    case CapabilityComparator::AtMost: return "AT_MOST";
    case CapabilityComparator::AnyOf: return "ANY_OF";
    case CapabilityComparator::AllOf: return "ALL_OF";
  }
  return "PRESENT";
}

bool parse_capability_comparator(std::string_view text, CapabilityComparator& out) noexcept {
  for (int i = 0; i <= static_cast<int>(CapabilityComparator::AllOf); ++i) {
    const auto candidate = static_cast<CapabilityComparator>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string CapabilityRequirement::to_string() const {
  std::string out = key.to_string();
  out += ' ';
  out += fo::to_string(comparator);
  if (value.known()) {
    out += ' ';
    out += value.to_string();
  }
  if (optional) {
    out += " [optional]";
  }
  if (!rationale.empty()) {
    out += " (";
    out += rationale;
    out += ')';
  }
  return out;
}

bool operator==(const CapabilityRequirement& a, const CapabilityRequirement& b) noexcept {
  return a.key == b.key && a.comparator == b.comparator && a.value == b.value &&
         a.optional == b.optional && a.rationale == b.rationale;
}

bool operator<(const CapabilityRequirement& a, const CapabilityRequirement& b) noexcept {
  if (a.key != b.key) return a.key < b.key;
  if (a.comparator != b.comparator) return a.comparator < b.comparator;
  return a.value < b.value;
}

bool operator<(const CapabilityCheck& a, const CapabilityCheck& b) noexcept {
  if (a.requirement != b.requirement) return a.requirement < b.requirement;
  return a.detail < b.detail;
}

CapabilityCheck evaluate_requirement(const CapabilityRequirement& requirement,
                                     const CapabilitySet& observed) {
  CapabilityCheck check;
  check.requirement = requirement;

  const CapabilityEntry* entry = observed.find(requirement.key);
  if (entry == nullptr) {
    check.satisfied = requirement.comparator == CapabilityComparator::Absent ? Tri::Yes : Tri::No;
    if (requirement.comparator != CapabilityComparator::Present &&
        requirement.comparator != CapabilityComparator::Absent) {
      check.satisfied = Tri::Unknown;
      check.detail = "capability not published; requirement cannot be evaluated";
      return check;
    }
    check.detail = requirement.comparator == CapabilityComparator::Absent
                       ? "capability is not published, as required"
                       : "capability is not published";
    return check;
  }

  check.observed = entry->value;
  if (!entry->value.known()) {
    check.satisfied = Tri::Unknown;
    check.detail = "capability is published without a known value";
    return check;
  }

  switch (requirement.comparator) {
    case CapabilityComparator::Present:
      if (entry->value.is_boolean()) {
        check.satisfied = tri_from_bool(entry->value.as_boolean(false));
        check.detail = check.satisfied == Tri::Yes ? "published and enabled"
                                                   : "published but disabled";
      } else {
        check.satisfied = Tri::Yes;
        check.detail = "published";
      }
      return check;
    case CapabilityComparator::Absent:
      if (entry->value.is_boolean()) {
        check.satisfied = tri_not(tri_from_bool(entry->value.as_boolean(true)));
        check.detail = check.satisfied == Tri::Yes ? "published and disabled" : "published and enabled";
      } else {
        check.satisfied = Tri::No;
        check.detail = "published with a value";
      }
      return check;
    case CapabilityComparator::Equals:
    case CapabilityComparator::NotEquals: {
      const Comparison numeric = compare_numeric_pairs(entry->value, requirement.value);
      bool equal = false;
      bool comparable = true;
      if (numeric.comparable) {
        equal = numeric.order == 0;
      } else if (entry->value.is_text() && requirement.value.is_text()) {
        equal = entry->value.as_text() == requirement.value.as_text();
      } else if (entry->value.is_text_set() && requirement.value.is_text()) {
        equal = text_set_contains(entry->value.as_text_set(), requirement.value.as_text());
      } else if (entry->value.is_boolean() && requirement.value.is_boolean()) {
        equal = entry->value.as_boolean(false) == requirement.value.as_boolean(true);
      } else {
        comparable = false;
      }
      if (!comparable) {
        check.satisfied = Tri::Unknown;
        check.detail = "published value and required value are not comparable";
        return check;
      }
      const bool result = requirement.comparator == CapabilityComparator::Equals ? equal : !equal;
      check.satisfied = tri_from_bool(result);
      check.detail = std::string("published ") + entry->value.to_string() +
                     (result ? " satisfies " : " violates ") + requirement.value.to_string();
      return check;
    }
    case CapabilityComparator::AtLeast:
    case CapabilityComparator::AtMost: {
      const Comparison numeric = compare_numeric_pairs(entry->value, requirement.value);
      bool result = false;
      if (numeric.comparable) {
        result = requirement.comparator == CapabilityComparator::AtLeast ? numeric.order >= 0
                                                                        : numeric.order <= 0;
      } else if (entry->value.is_text() && requirement.value.is_text()) {
        const int order = compare_version_strings(entry->value.as_text(), requirement.value.as_text());
        result = requirement.comparator == CapabilityComparator::AtLeast ? order >= 0 : order <= 0;
      } else {
        check.satisfied = Tri::Unknown;
        check.detail = "published value and required bound are not orderable";
        return check;
      }
      check.satisfied = tri_from_bool(result);
      check.detail = std::string("published ") + entry->value.to_string() +
                     (result ? " satisfies " : " violates ") + std::string(fo::to_string(requirement.comparator)) +
                     " " + requirement.value.to_string();
      return check;
    }
    case CapabilityComparator::AnyOf: {
      if (requirement.value.is_text_set()) {
        for (const std::string& candidate : requirement.value.as_text_set()) {
          if (entry->value.is_text() && entry->value.as_text() == candidate) {
            check.satisfied = Tri::Yes;
            check.detail = "published value matches an allowed alternative";
            return check;
          }
          if (entry->value.is_text_set() && text_set_contains(entry->value.as_text_set(), candidate)) {
            check.satisfied = Tri::Yes;
            check.detail = "published value set intersects the allowed alternatives";
            return check;
          }
        }
        check.satisfied = Tri::No;
        check.detail = "published value matches none of the allowed alternatives";
        return check;
      }
      check.satisfied = Tri::Unknown;
      check.detail = "ANY_OF requires a required value set";
      return check;
    }
    case CapabilityComparator::AllOf: {
      if (requirement.value.is_text_set()) {
        if (!entry->value.is_text_set()) {
          check.satisfied = Tri::No;
          check.detail = "published value is not a set";
          return check;
        }
        const CapabilityValue::TextSet& published = entry->value.as_text_set();
        for (const std::string& candidate : requirement.value.as_text_set()) {
          if (!text_set_contains(published, candidate)) {
            check.satisfied = Tri::No;
            check.detail = "published value set is missing " + candidate;
            return check;
          }
        }
        check.satisfied = Tri::Yes;
        check.detail = "published value set contains every required member";
        return check;
      }
      check.satisfied = Tri::Unknown;
      check.detail = "ALL_OF requires a required value set";
      return check;
    }
  }
  check.satisfied = Tri::Unknown;
  check.detail = "unrecognised comparator";
  return check;
}

std::vector<CapabilityCheck> evaluate_requirements(
    const std::vector<CapabilityRequirement>& requirements, const CapabilitySet& observed) {
  std::vector<CapabilityCheck> checks;
  checks.reserve(requirements.size());
  for (const CapabilityRequirement& requirement : requirements) {
    checks.push_back(evaluate_requirement(requirement, observed));
  }
  return checks;
}

int compare_version_strings(std::string_view a, std::string_view b) noexcept {
  std::size_t ia = 0;
  std::size_t ib = 0;
  while (ia < a.size() || ib < b.size()) {
    std::uint64_t na = 0;
    std::uint64_t nb = 0;
    bool a_numeric = false;
    bool b_numeric = false;
    while (ia < a.size() && a[ia] >= '0' && a[ia] <= '9') {
      a_numeric = true;
      na = na * 10 + static_cast<std::uint64_t>(a[ia] - '0');
      ++ia;
    }
    while (ib < b.size() && b[ib] >= '0' && b[ib] <= '9') {
      b_numeric = true;
      nb = nb * 10 + static_cast<std::uint64_t>(b[ib] - '0');
      ++ib;
    }
    if (a_numeric || b_numeric) {
      if (na != nb) {
        return na < nb ? -1 : 1;
      }
    }
    // Compare the separators/suffixes that follow the numeric runs.
    const char ca = ia < a.size() ? a[ia] : '\0';
    const char cb = ib < b.size() ? b[ib] : '\0';
    if (ca != cb) {
      // A missing component sorts before a present one, so "1.2" < "1.2.1".
      if (ca == '\0') return -1;
      if (cb == '\0') return 1;
      if (ca == '.' && cb != '.') return -1;
      if (cb == '.' && ca != '.') return 1;
      if (ca != cb) {
        return ca < cb ? -1 : 1;
      }
    }
    if (ia < a.size()) ++ia;
    if (ib < b.size()) ++ib;
  }
  return 0;
}

Result<std::vector<std::uint32_t>> parse_version(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "empty version string");
  }
  std::vector<std::uint32_t> parts;
  std::size_t index = 0;
  while (index <= text.size()) {
    const std::size_t dot = text.find('.', index);
    const std::string_view piece =
        dot == std::string_view::npos ? text.substr(index) : text.substr(index, dot - index);
    if (piece.empty()) {
      return Error(ErrorCode::InvalidArgument, "empty version component", std::string(text));
    }
    std::uint64_t value = 0;
    for (const char c : piece) {
      if (c < '0' || c > '9') {
        return Error(ErrorCode::InvalidArgument, "non-numeric version component",
                     std::string(piece));
      }
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
      if (value > UINT32_MAX) {
        return Error(ErrorCode::BoundExceeded, "version component out of range", std::string(piece));
      }
    }
    parts.push_back(static_cast<std::uint32_t>(value));
    if (parts.size() > 8) {
      return Error(ErrorCode::BoundExceeded, "version has too many components", std::string(text));
    }
    if (dot == std::string_view::npos) {
      break;
    }
    index = dot + 1;
  }
  return parts;
}

}  // namespace fo
