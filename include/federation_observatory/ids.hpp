// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "federation_observatory/codec.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"

namespace fo {

/// Maximum length of any identifier carried by the runtime, on the wire, or on disk.
/// Every decoder enforces this bound before allocating.
inline constexpr std::size_t kMaxIdentifierLength = 96;

/// Identifier alphabet: unreserved URI-ish characters only. Rejecting whitespace and
/// control characters keeps identifiers usable as map keys, file names and CLI tokens,
/// and makes truncated or corrupted input fail loudly instead of silently aliasing.
[[nodiscard]] FO_API bool is_valid_identifier(std::string_view text) noexcept;

/// A strongly typed identity. Two identities of different tag types are never
/// implicitly convertible, which is what keeps a ClusterId from being passed where a
/// SiteId is expected once generations start flowing through the same call.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() = default;
  explicit StrongId(std::string value) : value_(std::move(value)) {}

  /// Validate and construct. Rejects empty, over-long, or malformed identifiers.
  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    if (text.empty()) {
      return Error(ErrorCode::InvalidArgument, "empty identifier", std::string(Tag::name));
    }
    if (text.size() > kMaxIdentifierLength) {
      return Error(ErrorCode::BoundExceeded, "identifier too long", std::string(Tag::name));
    }
    if (!is_valid_identifier(text)) {
      return Error(ErrorCode::InvalidArgument, "identifier has invalid characters",
                   std::string(Tag::name));
    }
    return StrongId(std::string(text));
  }

  /// Bypass validation. Only valid for identifiers that were already validated, such
  /// as values produced by parsing the persistence format or the wire protocol.
  [[nodiscard]] static StrongId unchecked(std::string value) { return StrongId(std::move(value)); }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] const char* c_str() const noexcept { return value_.c_str(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  [[nodiscard]] std::size_t hash() const noexcept { return static_cast<std::size_t>(fnv1a64(value_)); }

  friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return a.value_ != b.value_; }
  friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.value_ < b.value_; }
  friend bool operator>(const StrongId& a, const StrongId& b) noexcept { return a.value_ > b.value_; }
  friend bool operator<=(const StrongId& a, const StrongId& b) noexcept { return a.value_ <= b.value_; }
  friend bool operator>=(const StrongId& a, const StrongId& b) noexcept { return a.value_ >= b.value_; }

 private:
  std::string value_;
};

#define FO_DEFINE_ID_TAG(TagName)          \
  struct TagName {                         \
    static constexpr const char* name = #TagName; \
  }

FO_DEFINE_ID_TAG(FederationIdTag);
FO_DEFINE_ID_TAG(SiteIdTag);
FO_DEFINE_ID_TAG(ClusterIdTag);
FO_DEFINE_ID_TAG(AcceleratorClassIdTag);
FO_DEFINE_ID_TAG(RuntimeIdTag);
FO_DEFINE_ID_TAG(BackendIdTag);
FO_DEFINE_ID_TAG(ArtifactIdTag);
FO_DEFINE_ID_TAG(WorkloadIdTag);
FO_DEFINE_ID_TAG(WorkloadClassIdTag);
FO_DEFINE_ID_TAG(PlacementIdTag);
FO_DEFINE_ID_TAG(MigrationIdTag);
FO_DEFINE_ID_TAG(PublisherIdTag);
FO_DEFINE_ID_TAG(PolicyIdTag);
FO_DEFINE_ID_TAG(CapabilityKeyIdTag);
FO_DEFINE_ID_TAG(DomainIdTag);
FO_DEFINE_ID_TAG(NodeIdTag);
FO_DEFINE_ID_TAG(ResourcePoolIdTag);

#undef FO_DEFINE_ID_TAG

using FederationId = StrongId<FederationIdTag>;
using SiteId = StrongId<SiteIdTag>;
using ClusterId = StrongId<ClusterIdTag>;
using AcceleratorClassId = StrongId<AcceleratorClassIdTag>;
using RuntimeId = StrongId<RuntimeIdTag>;
using BackendId = StrongId<BackendIdTag>;
using ArtifactId = StrongId<ArtifactIdTag>;
using WorkloadId = StrongId<WorkloadIdTag>;
using WorkloadClassId = StrongId<WorkloadClassIdTag>;
using PlacementId = StrongId<PlacementIdTag>;
using MigrationId = StrongId<MigrationIdTag>;
using PublisherId = StrongId<PublisherIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using CapabilityKeyId = StrongId<CapabilityKeyIdTag>;
/// A placement domain: the finest federation region inside which a single scheduler
/// may legally place co-dependent resources (usually one cluster, sometimes one site).
using DomainId = StrongId<DomainIdTag>;
using NodeId = StrongId<NodeIdTag>;
using ResourcePoolId = StrongId<ResourcePoolIdTag>;

/// A generation numbers a body of state that can become stale or change semantics.
/// Zero is reserved and always means "generation unset / unknown"; it never compares
/// newer than a real generation and never satisfies a freshness requirement.
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;

  Generation() = default;
  explicit constexpr Generation(std::uint64_t value) : value_(value) {}

  [[nodiscard]] static constexpr Generation unset() noexcept { return Generation(0); }
  [[nodiscard]] static constexpr Generation first() noexcept { return Generation(1); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == 0; }

  /// Deterministic successor. Saturates at the maximum representable generation
  /// instead of wrapping, so a fenced comparison can never be defeated by overflow.
  [[nodiscard]] constexpr Generation next() const noexcept {
    return value_ == UINT64_MAX ? Generation(value_) : Generation(value_ + 1);
  }

  [[nodiscard]] constexpr bool newer_than(const Generation& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool older_than(const Generation& other) const noexcept {
    return value_ < other.value_;
  }

  friend constexpr bool operator==(const Generation& a, const Generation& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Generation& a, const Generation& b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(const Generation& a, const Generation& b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(const Generation& a, const Generation& b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator<=(const Generation& a, const Generation& b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>=(const Generation& a, const Generation& b) noexcept {
    return a.value_ >= b.value_;
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

#define FO_DEFINE_GENERATION_TAG(TagName) \
  struct TagName {                        \
    static constexpr const char* name = #TagName; \
  }

FO_DEFINE_GENERATION_TAG(FederationGenerationTag);
FO_DEFINE_GENERATION_TAG(SiteGenerationTag);
FO_DEFINE_GENERATION_TAG(ClusterGenerationTag);
FO_DEFINE_GENERATION_TAG(ClusterEpochTag);
FO_DEFINE_GENERATION_TAG(AcceleratorCapabilityGenerationTag);
FO_DEFINE_GENERATION_TAG(RuntimeGenerationTag);
FO_DEFINE_GENERATION_TAG(BackendGenerationTag);
FO_DEFINE_GENERATION_TAG(ArtifactGenerationTag);
FO_DEFINE_GENERATION_TAG(WorkloadGenerationTag);
FO_DEFINE_GENERATION_TAG(PlacementGenerationTag);
FO_DEFINE_GENERATION_TAG(MigrationGenerationTag);
FO_DEFINE_GENERATION_TAG(PolicyGenerationTag);
FO_DEFINE_GENERATION_TAG(CompatibilityGenerationTag);
FO_DEFINE_GENERATION_TAG(CapacityGenerationTag);
FO_DEFINE_GENERATION_TAG(TopologyGenerationTag);
FO_DEFINE_GENERATION_TAG(EvidenceGenerationTag);
FO_DEFINE_GENERATION_TAG(CoordinatorEpochTag);
FO_DEFINE_GENERATION_TAG(SnapshotGenerationTag);
FO_DEFINE_GENERATION_TAG(BootGenerationTag);
FO_DEFINE_GENERATION_TAG(PublisherGenerationTag);

#undef FO_DEFINE_GENERATION_TAG

using FederationGeneration = Generation<FederationGenerationTag>;
using SiteGeneration = Generation<SiteGenerationTag>;
using ClusterGeneration = Generation<ClusterGenerationTag>;
/// Incremented every time a cluster's *process/agent incarnation* restarts while the
/// stable ClusterId survives. Capacity and liveness observed under an older epoch is
/// not evidence about the current incarnation.
using ClusterEpoch = Generation<ClusterEpochTag>;
using AcceleratorCapabilityGeneration = Generation<AcceleratorCapabilityGenerationTag>;
using RuntimeGeneration = Generation<RuntimeGenerationTag>;
using BackendGeneration = Generation<BackendGenerationTag>;
using ArtifactGeneration = Generation<ArtifactGenerationTag>;
using WorkloadGeneration = Generation<WorkloadGenerationTag>;
using PlacementGeneration = Generation<PlacementGenerationTag>;
using MigrationGeneration = Generation<MigrationGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using CompatibilityGeneration = Generation<CompatibilityGenerationTag>;
using CapacityGeneration = Generation<CapacityGenerationTag>;
using TopologyGeneration = Generation<TopologyGenerationTag>;
using EvidenceGeneration = Generation<EvidenceGenerationTag>;
/// Advanced on every coordinator start. Frames stamped with an older epoch belong to a
/// dead coordinator incarnation and must never mutate recovered state.
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using SnapshotGeneration = Generation<SnapshotGenerationTag>;
/// Identity of one publisher process incarnation. A boot generation is never reused.
using BootGeneration = Generation<BootGenerationTag>;
using PublisherGeneration = Generation<PublisherGenerationTag>;

/// Per-publisher monotonic event counter. Transport arrival order is never trusted.
class FO_API Sequence {
 public:
  Sequence() = default;
  explicit constexpr Sequence(std::uint64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr Sequence next() const noexcept {
    return value_ == UINT64_MAX ? Sequence(value_) : Sequence(value_ + 1);
  }

  friend constexpr bool operator==(Sequence a, Sequence b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Sequence a, Sequence b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Sequence a, Sequence b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Sequence a, Sequence b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator<=(Sequence a, Sequence b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(Sequence a, Sequence b) noexcept { return a.value_ >= b.value_; }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

/// Nanoseconds since the Unix epoch, or 0 when the observation carried no time.
using TimestampNanos = std::int64_t;

/// Current wall clock in nanoseconds since the Unix epoch. Used only to stamp local
/// observations; it is never used as an ordering authority between publishers.
[[nodiscard]] FO_API TimestampNanos now_unix_nanos() noexcept;

/// Stable, sortable rendering of a UTC nanosecond timestamp. Deterministic.
[[nodiscard]] FO_API std::string format_timestamp(TimestampNanos nanos);

/// Render an identifier for human output, substituting a marker for the empty id.
[[nodiscard]] FO_API std::string render_id(std::string_view id);
[[nodiscard]] FO_API std::string_view render_unset_generation() noexcept;

}  // namespace fo
