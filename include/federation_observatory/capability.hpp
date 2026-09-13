// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"

namespace fo {

/// Capability categories. A category names the *kind of question* a capability key
/// answers, so that aggregate mismatch analysis can group findings without string
/// matching.
enum class CapabilityCategory : std::uint8_t {
  AcceleratorArchitecture = 0,
  PrecisionSupport,
  MemorySize,
  MemoryBandwidthClass,
  Interconnect,
  PartitionSupport,
  RuntimeApi,
  DriverApi,
  CompilerTarget,
  KernelFormat,
  CollectiveSupport,
  RdmaCapability,
  GpuDirectCapability,
  CxlCapability,
  DpuSmartNicCapability,
  ArtifactFormat,
  QuantizationSupport,
  SparseExecutionSupport,
  FirmwareGeneration,
  ContainerRuntime,
  OperatingSystem,
  SecurityIsolation,
  OffloadCapability,
  Custom,
};

[[nodiscard]] FO_API std::string_view to_string(CapabilityCategory category) noexcept;
[[nodiscard]] FO_API bool parse_capability_category(std::string_view text,
                                                    CapabilityCategory& out) noexcept;

/// Typed, enumerated capability keys. New keys are added to this enumeration rather
/// than smuggled in as free-form strings; sites that need vendor-private capability
/// names use CapabilityKey::Custom together with a validated CapabilityKeyId.
enum class CapabilityKey : std::uint16_t {
  Unknown = 0,
  // AcceleratorArchitecture
  AcceleratorArchitecture = 1,
  AcceleratorVendor = 2,
  AcceleratorFamily = 3,
  AcceleratorModel = 4,
  ComputeCapability = 5,
  // PrecisionSupport
  PrecisionFp64 = 20,
  PrecisionFp32 = 21,
  PrecisionTf32 = 22,
  PrecisionFp16 = 23,
  PrecisionBf16 = 24,
  PrecisionFp8E4M3 = 25,
  PrecisionFp8E5M2 = 26,
  PrecisionFp6 = 27,
  PrecisionInt8 = 28,
  PrecisionInt4 = 29,
  PrecisionFp4 = 30,
  // MemorySize / MemoryBandwidthClass
  MemoryBytes = 40,
  MemoryBandwidthGbps = 41,
  MemoryType = 42,
  MemoryEcc = 43,
  // Interconnect
  InterconnectType = 60,
  InterconnectBandwidthGbps = 61,
  NvlinkGeneration = 62,
  PcieGeneration = 63,
  // PartitionSupport
  PartitionSupport = 80,
  PartitionModes = 81,
  PartitionGeometry = 82,
  PartitionMaxInstances = 83,
  // RuntimeApi / DriverApi
  RuntimeApi = 100,
  RuntimeVersion = 101,
  RuntimeAbi = 102,
  RuntimeGeneration = 103,
  DriverApi = 104,
  DriverVersion = 105,
  DriverAbi = 106,
  DriverGeneration = 107,
  // CompilerTarget / KernelFormat
  CompilerTarget = 120,
  CompilerVersion = 121,
  KernelFormat = 122,
  KernelArchitectures = 123,
  // CollectiveSupport
  CollectiveSupport = 140,
  CollectiveBackend = 141,
  CollectiveMaxGroupSize = 142,
  // Fabric capabilities
  RdmaCapability = 160,
  GpuDirectCapability = 161,
  GpuDirectStorage = 162,
  CxlCapability = 163,
  DpuSmartNicCapability = 164,
  // Artifact-shaped capabilities
  ArtifactFormat = 180,
  ArtifactFormats = 181,
  QuantizationSupport = 182,
  QuantizationFormats = 183,
  SparseExecutionSupport = 184,
  InferenceServingBackend = 185,
  // Environment
  FirmwareGeneration = 200,
  ContainerRuntime = 201,
  OperatingSystem = 202,
  SecurityIsolation = 203,
  OffloadCapability = 204,
  // Extensibility
  Custom = 4096,
};

/// Total number of enumerators in CapabilityKey that carry meaning.
inline constexpr std::size_t kCapabilityKeyCount = 57;

[[nodiscard]] FO_API std::string_view to_string(CapabilityKey key) noexcept;
[[nodiscard]] FO_API CapabilityCategory category_of(CapabilityKey key) noexcept;
[[nodiscard]] FO_API bool parse_capability_key(std::string_view text, CapabilityKey& out) noexcept;

/// A capability key as carried by publications and requirements: either one of the
/// typed keys, or a validated vendor extension identifier.
struct FO_API CapabilityRef {
  CapabilityKey key = CapabilityKey::Unknown;
  CapabilityKeyId custom;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] CapabilityCategory category() const noexcept;
  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const CapabilityRef& a, const CapabilityRef& b) noexcept;
  friend bool operator<(const CapabilityRef& a, const CapabilityRef& b) noexcept;
};

/// Bounded, typed capability value.
class FO_API CapabilityValue {
 public:
  using TextSet = std::vector<std::string>;
  using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string,
                               TextSet>;

  CapabilityValue() = default;

  [[nodiscard]] static CapabilityValue boolean(bool v);
  [[nodiscard]] static CapabilityValue integer(std::int64_t v);
  [[nodiscard]] static CapabilityValue unsigned_integer(std::uint64_t v);
  [[nodiscard]] static CapabilityValue number(double v);
  [[nodiscard]] static CapabilityValue text(std::string v);
  [[nodiscard]] static Result<CapabilityValue> text_set(TextSet values);

  [[nodiscard]] bool known() const noexcept { return storage_.index() != 0; }
  [[nodiscard]] bool is_boolean() const noexcept { return std::holds_alternative<bool>(storage_); }
  [[nodiscard]] bool is_integral() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_) ||
           std::holds_alternative<std::uint64_t>(storage_);
  }
  [[nodiscard]] bool is_number() const noexcept { return std::holds_alternative<double>(storage_); }
  [[nodiscard]] bool is_text() const noexcept { return std::holds_alternative<std::string>(storage_); }
  [[nodiscard]] bool is_text_set() const noexcept { return std::holds_alternative<TextSet>(storage_); }

  [[nodiscard]] bool as_boolean(bool fallback) const noexcept;
  [[nodiscard]] std::int64_t as_integer(std::int64_t fallback) const noexcept;
  [[nodiscard]] std::uint64_t as_unsigned(std::uint64_t fallback) const noexcept;
  [[nodiscard]] double as_number(double fallback) const noexcept;
  [[nodiscard]] std::string_view as_text() const noexcept;
  [[nodiscard]] const TextSet& as_text_set() const noexcept;

  /// Deterministic rendering. Numbers use integer formatting when exactly integral.
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const CapabilityValue& a, const CapabilityValue& b) noexcept;
  [[nodiscard]] friend bool operator<(const CapabilityValue& a, const CapabilityValue& b) noexcept;

 private:
  Storage storage_;
};

/// A single published capability of a cluster, accelerator class, runtime or backend.
struct FO_API CapabilityEntry {
  CapabilityRef key;
  CapabilityValue value;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceGeneration generation;
  std::string note;

  friend bool operator==(const CapabilityEntry& a, const CapabilityEntry& b) noexcept;
  friend bool operator<(const CapabilityEntry& a, const CapabilityEntry& b) noexcept;
};

/// An ordered, deduplicated set of capability entries. Ordering is by CapabilityRef so
/// that serialization is canonical without consulting insertion order.
class FO_API CapabilitySet {
 public:
  CapabilitySet() = default;
  explicit CapabilitySet(std::size_t limit) : limit_(limit) {}

  [[nodiscard]] Status put(CapabilityEntry entry);
  [[nodiscard]] const CapabilityEntry* find(const CapabilityRef& key) const noexcept;
  [[nodiscard]] bool contains(const CapabilityRef& key) const noexcept { return find(key) != nullptr; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<CapabilityEntry>& entries() const noexcept { return entries_; }

  void set_limit(std::size_t limit) noexcept { limit_ = limit; }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

  /// Weakest precision across published entries, or Unknown when empty.
  [[nodiscard]] Precision combined_precision() const noexcept;
  [[nodiscard]] EvidenceClass combined_class() const noexcept;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;

 private:
  std::vector<CapabilityEntry> entries_;
  std::size_t limit_ = kMaxCapabilityEntriesPerPublication;
};

/// How a requirement compares a published capability value.
enum class CapabilityComparator : std::uint8_t {
  Present = 0,
  Absent,
  Equals,
  NotEquals,
  AtLeast,
  AtMost,
  AnyOf,
  AllOf,
};

[[nodiscard]] FO_API std::string_view to_string(CapabilityComparator comparator) noexcept;
[[nodiscard]] FO_API bool parse_capability_comparator(std::string_view text,
                                                      CapabilityComparator& out) noexcept;

/// One requirement a workload, artifact or portability class places on a target.
struct FO_API CapabilityRequirement {
  CapabilityRef key;
  CapabilityComparator comparator = CapabilityComparator::Present;
  CapabilityValue value;
  /// When true, an unsatisfied requirement is recorded but does not by itself make the
  /// target incompatible.
  bool optional = false;
  std::string rationale;

  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const CapabilityRequirement& a, const CapabilityRequirement& b) noexcept;
  friend bool operator<(const CapabilityRequirement& a, const CapabilityRequirement& b) noexcept;
};

/// Result of comparing one requirement against one capability set.
struct FO_API CapabilityCheck {
  CapabilityRequirement requirement;
  CapabilityValue observed;
  Tri satisfied = Tri::Unknown;
  std::string detail;

  friend bool operator<(const CapabilityCheck& a, const CapabilityCheck& b) noexcept;
};

/// Evaluate one requirement against a capability set. The result is Unknown whenever
/// the published evidence does not decide the question: absence of evidence is never
/// treated as evidence of absence.
[[nodiscard]] FO_API CapabilityCheck evaluate_requirement(const CapabilityRequirement& requirement,
                                                          const CapabilitySet& observed);

/// Evaluate a list of requirements in deterministic order.
[[nodiscard]] FO_API std::vector<CapabilityCheck> evaluate_requirements(
    const std::vector<CapabilityRequirement>& requirements, const CapabilitySet& observed);

/// Compare two dotted version strings ("12.9.1" against "12.10"). Missing components
/// compare as zero. Non-numeric suffixes are compared lexicographically after the
/// numeric prefix so that "12.9-rc1" is ordered deterministically.
[[nodiscard]] FO_API int compare_version_strings(std::string_view a, std::string_view b) noexcept;

/// Parse a dotted version into numeric components. Returns an error for empty input.
[[nodiscard]] FO_API Result<std::vector<std::uint32_t>> parse_version(std::string_view text);

}  // namespace fo
