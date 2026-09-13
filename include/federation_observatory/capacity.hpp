// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"

namespace fo {

/// What a capacity pool counts.
enum class ResourceKind : std::uint8_t {
  Accelerator = 0,  ///< Devices, counted individually.
  MemoryBytes = 1,  ///< Bytes of device-visible memory.
  Custom = 2,       ///< An externally named resource kind.
};

[[nodiscard]] FO_API std::string_view to_string(ResourceKind kind) noexcept;
[[nodiscard]] FO_API bool parse_resource_kind(std::string_view text, ResourceKind& out) noexcept;

/// Exact, checked capacity decomposition.
///
/// Accounting identity, enforced on construction and re-verified by validate():
///
///   nominal = offline + allocated + reserved + draining + unusable + idle
///
/// with an invariant of construction
///
///   online  = nominal - offline
///           = allocated + reserved + draining + unusable + idle
///
/// * offline   - physically present but not powered/registered/serving.
/// * allocated - committed to a workload by an owning scheduler (observed, not owned).
/// * reserved  - withheld from general placement by reservation (observed).
/// * draining  - online but being evacuated; not eligible for new placement.
/// * unusable  - online but not schedulable for health/readiness reasons.
/// * idle      - online, healthy, not committed, not draining. This is the only pool
///               that eligibility analysis may draw from. Idle is NOT stranded;
///               idle becomes stranded only when a stated workload class cannot
///               legally consume it.
///
/// The buckets above are mutually exclusive and sum exactly to nominal. No other
/// category in the runtime is allowed to be added into them: eligibility and
/// stranding are computed as a *partition of idle*, never as extra buckets here.
struct FO_API CapacityLedger {
  std::uint64_t nominal = 0;
  std::uint64_t offline = 0;
  std::uint64_t allocated = 0;
  std::uint64_t reserved = 0;
  std::uint64_t draining = 0;
  std::uint64_t unusable = 0;
  std::uint64_t idle = 0;

  /// Build a ledger from the observable components, deriving idle. Fails with
  /// CapacityInconsistent when the components exceed nominal or when the checked
  /// arithmetic overflows.
  [[nodiscard]] static Result<CapacityLedger> from_components(std::uint64_t nominal,
                                                              std::uint64_t offline,
                                                              std::uint64_t allocated,
                                                              std::uint64_t reserved,
                                                              std::uint64_t draining,
                                                              std::uint64_t unusable);

  [[nodiscard]] std::uint64_t online() const noexcept;
  [[nodiscard]] std::uint64_t committed() const noexcept;  ///< allocated + reserved + draining
  [[nodiscard]] Status validate() const;

  friend bool operator==(const CapacityLedger& a, const CapacityLedger& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// One capacity pool inside one cluster.
struct FO_API CapacityPool {
  ResourcePoolId pool_id;
  ResourceKind kind = ResourceKind::Accelerator;
  /// Set when kind == Accelerator; names the device class this pool counts.
  AcceleratorClassId accelerator_class;
  /// Set when kind == Custom.
  std::string custom_name;
  CapacityLedger ledger;
  CapacityGeneration generation;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  EvidenceList evidence;

  friend bool operator==(const CapacityPool& a, const CapacityPool& b) noexcept;
  friend bool operator<(const CapacityPool& a, const CapacityPool& b) noexcept;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Why nominally-available capacity cannot be consumed by a stated workload class.
/// These are the stranding causes the runtime distinguishes. They are not synonyms:
/// a runtime mismatch and a policy restriction produce the same *amount* stranded but
/// a different *finding*, and downstream consumers act on them differently.
enum class StrandingReason : std::uint8_t {
  None = 0,
  UnsupportedAcceleratorCapability = 1,
  UnsupportedPrecision = 2,
  MissingOffloadCapability = 3,
  MissingInterconnectFeature = 4,
  RuntimeMismatch = 5,
  DriverMismatch = 6,
  AbiMismatch = 7,
  ArtifactIncompatibility = 8,
  MissingCompilerTarget = 9,
  PartitionGeometry = 10,
  TopologyConstraint = 11,
  IsolationRequirement = 12,
  MissingLocalStorageState = 13,
  MemoryInsufficient = 14,
  SiteRestriction = 15,
  PolicyRestriction = 16,
  Unknown = 17,
};

inline constexpr std::size_t kStrandingReasonCount = 18;

[[nodiscard]] FO_API std::string_view to_string(StrandingReason reason) noexcept;
[[nodiscard]] FO_API bool parse_stranding_reason(std::string_view text, StrandingReason& out) noexcept;

/// Deterministic precedence used to assign a single *primary* reason to a capacity
/// unit so that reason amounts can be summed without double counting. The order runs
/// from the most specific technical cause to the broadest policy cause.
[[nodiscard]] FO_API std::size_t stranding_reason_precedence(StrandingReason reason) noexcept;

/// True when the reason describes a technical incompatibility rather than a policy or
/// administrative restriction. Downstream consumers must not conflate the two.
[[nodiscard]] FO_API bool is_technical_reason(StrandingReason reason) noexcept;

/// Allocation-free tallies indexed by StrandingReason. Amounts are added with checked
/// arithmetic; overflow is reported rather than wrapped.
class FO_API ReasonTally {
 public:
  ReasonTally() = default;

  [[nodiscard]] Status add(StrandingReason reason, std::uint64_t amount) noexcept;
  [[nodiscard]] std::uint64_t get(StrandingReason reason) const noexcept;
  [[nodiscard]] std::uint64_t total() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return total() == 0; }
  void clear() noexcept { amounts_.fill(0); }

  /// Non-zero entries in deterministic StrandingReason order.
  [[nodiscard]] std::vector<std::pair<StrandingReason, std::uint64_t>> non_zero() const;

 private:
  std::array<std::uint64_t, kStrandingReasonCount> amounts_{};
};

/// Partition of the idle capacity of a scope, for one stated workload class and one
/// resource kind.
///
///   idle = usable + unknown + sum(stranded_by_primary_reason)
///
/// * usable  - idle capacity that satisfies every stated requirement.
/// * unknown - idle capacity for which evidence is insufficient to decide. This is a
///             first-class outcome; it is never folded into usable or stranded.
/// * stranded - idle capacity that is provably not consumable by the workload class.
///              Each unit is assigned exactly one primary reason, so the primary
///              tallies sum to the total without double counting.
/// * occurrences - overlapping view: a unit that fails three requirements is counted
///             once in each reason's occurrence tally. Occurrence tallies therefore do
///             NOT sum to the stranded total and are documented as overlapping.
///
/// Fragmentation is deliberately absent from this structure. Fragmentation describes
/// how usable capacity is distributed across placement domains and is reported as a
/// separate orthogonal finding; it never adds to a stranding amount.
struct FO_API EligibilityBreakdown {
  std::uint64_t idle = 0;
  std::uint64_t usable = 0;
  std::uint64_t unknown = 0;
  ReasonTally stranded_by_primary_reason;
  ReasonTally reason_occurrences;

  [[nodiscard]] std::uint64_t stranded() const noexcept;
  /// True when idle == usable + unknown + stranded() and the tallies are consistent.
  [[nodiscard]] bool closes() const noexcept;
  [[nodiscard]] Status validate() const;
  /// Percentage of idle that is stranded, as an exact two-decimal rendering.
  [[nodiscard]] std::string stranded_percent_of_idle() const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;

  void merge(const EligibilityBreakdown& other);
};

/// Nominal-versus-usable summary for one scope, without per-reason detail.
struct FO_API CapacitySummary {
  std::uint64_t nominal = 0;
  std::uint64_t offline = 0;
  std::uint64_t allocated = 0;
  std::uint64_t reserved = 0;
  std::uint64_t draining = 0;
  std::uint64_t unusable = 0;
  std::uint64_t idle = 0;
  std::uint64_t usable = 0;
  std::uint64_t stranded = 0;
  std::uint64_t unknown = 0;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

}  // namespace fo
