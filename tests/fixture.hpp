// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Shared test fixtures. Everything here goes through the public API only.

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "federation_observatory/observatory.hpp"
#include "harness.hpp"

namespace fixture {

inline constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

/// Require that a publication was actually applied. A Result<IngestResult> that is
/// "ok" only means the coordinator processed the request; the disposition is what says
/// whether state changed. Fixtures assert the disposition so that a silently rejected
/// publication can never make a test pass for the wrong reason.
inline void require_applied(const fo::Result<fo::IngestResult>& result, const char* what) {
  if (!result.ok()) {
    ::fotest::fail(__FILE__, __LINE__,
                   std::string(what) + " failed: " + result.error().to_string());
  }
  if (result.value().disposition != fo::IngestDisposition::Applied) {
    ::fotest::fail(__FILE__, __LINE__,
                   std::string(what) + " was not applied: " +
                       std::string(fo::to_string(result.value().disposition)) + " (" +
                       result.value().detail + ")");
  }
}

/// A publisher identity plus a monotonically increasing sequence counter.
struct Publisher {
  /// A publisher owns exactly one ordered stream, so its sequence counter is guarded: two
  /// threads sharing one identity would otherwise interleave allocations and produce
  /// out-of-order sequence numbers, which is a publisher bug rather than a runtime one.
  mutable std::mutex sequence_mutex;
  fo::PublisherId id{"test-publisher"};
  fo::BootGeneration boot{1};
  fo::CoordinatorEpoch epoch{1};
  fo::FederationId federation{"test-fed"};
  fo::Sequence sequence;
  fo::EvidenceGeneration evidence{1};
  fo::Precision precision = fo::Precision::Exact;
  fo::EvidenceClass evidence_class = fo::EvidenceClass::Synthetic;
  fo::Provenance provenance = fo::Provenance::SyntheticBackend;

  Publisher() = default;
  explicit Publisher(std::string identifier)
      : id(fo::PublisherId::unchecked(std::move(identifier))) {}

  [[nodiscard]] fo::PublicationContext next(
      fo::FederationGeneration federation_generation = fo::FederationGeneration{1}) {
    const std::lock_guard<std::mutex> lock(sequence_mutex);
    fo::PublicationContext context;
    context.publisher = id;
    context.boot = boot;
    context.coordinator_epoch = epoch;
    context.federation = federation;
    context.federation_generation = federation_generation;
    sequence = sequence.next();
    context.sequence = sequence;
    context.observed_at = fo::now_unix_nanos();
    context.precision = precision;
    context.evidence_class = evidence_class;
    context.provenance = provenance;
    context.evidence_generation = evidence;
    return context;
  }

  /// A context that re-uses the previous sequence number, for duplicate tests.
  [[nodiscard]] fo::PublicationContext repeat(
      fo::FederationGeneration federation_generation = fo::FederationGeneration{1}) {
    const std::lock_guard<std::mutex> lock(sequence_mutex);
    fo::PublicationContext context;
    context.publisher = id;
    context.boot = boot;
    context.coordinator_epoch = epoch;
    context.federation = federation;
    context.federation_generation = federation_generation;
    context.sequence = sequence;
    context.observed_at = fo::now_unix_nanos();
    context.precision = precision;
    context.evidence_class = evidence_class;
    context.provenance = provenance;
    context.evidence_generation = evidence;
    return context;
  }
};

/// The capability set a real publisher would declare for an accelerator family: the
/// architecture, the precisions it supports, the artifact and kernel formats it accepts,
/// and the compiler target it is built for. A publisher that declares none of these
/// leaves the analyser unable to decide, which is correct but not what a fixture wants.
[[nodiscard]] fo::CapabilitySet family_capabilities(std::size_t family);

/// One accelerator family description used by the fixtures.
struct Family {
  const char* name = "family-a";
  const char* architecture = "sm_90";
  const char* compute_capability = "9.0";
  std::uint64_t memory_bytes = 80 * kGiB;
  bool fp8 = true;
  std::uint32_t devices = 4;
};

/// Builds a small but complete federation through the public API.
class World {
 public:
  explicit World(fo::FederationObservatory& observatory) : observatory_(&observatory) {}

  [[nodiscard]] fo::Status build(std::size_t cluster_count = 2,
                                 std::size_t family_count = 2);

  [[nodiscard]] fo::FederationObservatory& observatory() { return *observatory_; }
  [[nodiscard]] Publisher& publisher() { return publisher_; }
  [[nodiscard]] const std::vector<fo::ClusterId>& clusters() const { return clusters_; }
  [[nodiscard]] const std::vector<fo::AcceleratorClassId>& classes() const { return classes_; }
  [[nodiscard]] const std::vector<fo::RuntimeId>& runtimes() const { return runtimes_; }
 private:
  fo::FederationObservatory* observatory_ = nullptr;
  Publisher publisher_;
  std::vector<fo::ClusterId> clusters_;
  std::vector<fo::AcceleratorClassId> classes_;
  std::vector<fo::RuntimeId> runtimes_;
  std::vector<fo::SiteId> sites_;
};

}  // namespace fixture
