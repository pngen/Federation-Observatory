// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"

namespace fo {

/// Tri-state truth. The runtime never collapses "we do not know" into "no".
enum class Tri : std::uint8_t {
  No = 0,
  Unknown = 1,
  Yes = 2,
};

[[nodiscard]] FO_API std::string_view to_string(Tri value) noexcept;
[[nodiscard]] FO_API Tri tri_and(Tri a, Tri b) noexcept;
[[nodiscard]] FO_API Tri tri_or(Tri a, Tri b) noexcept;
[[nodiscard]] FO_API Tri tri_not(Tri a) noexcept;
[[nodiscard]] FO_API Tri tri_from_bool(bool value) noexcept;

/// How precisely a finding is known. Ordered from strongest to weakest.
enum class Precision : std::uint8_t {
  Exact = 0,       ///< Reported by the authoritative source for the exact generation.
  Aggregated = 1,  ///< Summed over a stated population; per-member detail not retained.
  Sampled = 2,     ///< Extrapolated from a stated sample.
  Derived = 3,     ///< Computed deterministically from exact inputs.
  Inferred = 4,    ///< Consistent with evidence but not stated by any source.
  Ambiguous = 5,   ///< Evidence supports more than one mutually exclusive reading.
  Unknown = 6,     ///< No usable evidence.
};

[[nodiscard]] FO_API std::string_view to_string(Precision precision) noexcept;
/// Numeric rank; larger means weaker. Used to combine evidence without optimism.
[[nodiscard]] FO_API int precision_rank(Precision precision) noexcept;
[[nodiscard]] FO_API Precision weakest(Precision a, Precision b) noexcept;
[[nodiscard]] FO_API Precision strongest(Precision a, Precision b) noexcept;
[[nodiscard]] FO_API bool parse_precision(std::string_view text, Precision& out) noexcept;

/// Whether a stated reason was observed directly or produced by the runtime.
enum class ReasonBasis : std::uint8_t {
  Observed = 0,   ///< The source stated this reason.
  Derived = 1,    ///< Computed from observed facts by a deterministic rule.
  Inferred = 2,   ///< Consistent with evidence; no source stated it.
  Unattributed = 3,  ///< No reason is supportable. First-class outcome.
};

[[nodiscard]] FO_API std::string_view to_string(ReasonBasis basis) noexcept;
[[nodiscard]] FO_API int reason_basis_rank(ReasonBasis basis) noexcept;
[[nodiscard]] FO_API ReasonBasis weakest(ReasonBasis a, ReasonBasis b) noexcept;

/// REAL / SYNTHETIC / UNSUPPORTED classification of an evidence source.
enum class EvidenceClass : std::uint8_t {
  Real = 0,         ///< Produced by physical hardware, a real runtime, or a real process.
  Synthetic = 1,    ///< Produced by the deterministic synthetic backend.
  Unsupported = 2,  ///< The environment cannot produce this evidence at all.
  Unknown = 3,      ///< The source did not declare its class.
};

[[nodiscard]] FO_API std::string_view to_string(EvidenceClass cls) noexcept;
[[nodiscard]] FO_API int evidence_class_rank(EvidenceClass cls) noexcept;
[[nodiscard]] FO_API EvidenceClass weaker(EvidenceClass a, EvidenceClass b) noexcept;
[[nodiscard]] FO_API bool parse_evidence_class(std::string_view text, EvidenceClass& out) noexcept;

/// Where a piece of evidence came from.
enum class Provenance : std::uint8_t {
  HostProbe = 0,               ///< Real local machine discovery by this runtime.
  ToolchainProbe = 1,          ///< Real local compiler/runtime toolchain inspection.
  ArtifactInspection = 2,      ///< Real artifact metadata read from a real artifact file.
  FederationController = 3,
  ClusterController = 4,
  Scheduler = 5,
  RuntimeRegistry = 6,
  HardwareCapabilityRegistry = 7,
  ArtifactRegistry = 8,
  MigrationRuntime = 9,
  ResourceBroker = 10,
  ExternalInventory = 11,
  SyntheticBackend = 12,
  ImportedTrace = 13,
  DerivedAnalysis = 14,
  PublisherReport = 15,
  Unknown = 16,
};

[[nodiscard]] FO_API std::string_view to_string(Provenance provenance) noexcept;
[[nodiscard]] FO_API bool parse_provenance(std::string_view text, Provenance& out) noexcept;

/// One traceable piece of supporting evidence.
struct FO_API EvidenceRef {
  Provenance source = Provenance::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unknown;
  std::string source_id;
  EvidenceGeneration generation;
  Precision precision = Precision::Unknown;
  std::string detail;

  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const EvidenceRef& a, const EvidenceRef& b) noexcept;
  friend bool operator<(const EvidenceRef& a, const EvidenceRef& b) noexcept;
};

/// A bounded, deterministically ordered collection of evidence references. Insertion
/// deduplicates by value so that replaying the same observation twice does not inflate
/// the supporting evidence of a finding.
class FO_API EvidenceList {
 public:
  EvidenceList() = default;
  explicit EvidenceList(std::size_t limit) : limit_(limit) {}

  [[nodiscard]] Status add(EvidenceRef ref);
  [[nodiscard]] Status add(Provenance source, EvidenceClass cls, std::string source_id,
                           Precision precision, std::string detail = {});

  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] const std::vector<EvidenceRef>& items() const noexcept { return items_; }

  /// Weakest precision among the contained evidence. Unknown when empty.
  [[nodiscard]] Precision combined_precision() const noexcept;
  /// Weakest evidence class among the contained evidence. Unknown when empty.
  [[nodiscard]] EvidenceClass combined_class() const noexcept;
  /// True when every contained reference is classified as REAL.
  [[nodiscard]] bool all_real() const noexcept;
  /// True when any contained reference is classified as SYNTHETIC.
  [[nodiscard]] bool any_synthetic() const noexcept;

  /// Deterministic multi-line rendering, one reference per line.
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;

  void set_limit(std::size_t limit) noexcept { limit_ = limit; }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

 private:
  std::vector<EvidenceRef> items_;
  std::size_t limit_ = kDefaultEvidencePerFinding;
};

/// Deterministic fingerprint of the evidence content, used to prove that identical
/// inputs produce identical explanations.
[[nodiscard]] FO_API std::string evidence_digest(const EvidenceList& evidence);

}  // namespace fo
