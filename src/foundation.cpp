// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Deterministic primitives: checked arithmetic, exact fixed-point ratios, canonical
// binary encoding, identity and generation validation, evidence classification, and the
// text rendering helpers every renderer shares.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/codec.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/version.hpp"

namespace fo {
namespace {

// ---------------------------------------------------------------------------
// 128-bit helpers, used to keep percentage arithmetic exact without floating point.
// ---------------------------------------------------------------------------

struct U128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

U128 mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a0 = a & 0xFFFFFFFFull;
  const std::uint64_t a1 = a >> 32;
  const std::uint64_t b0 = b & 0xFFFFFFFFull;
  const std::uint64_t b1 = b >> 32;

  const std::uint64_t p00 = a0 * b0;
  const std::uint64_t p01 = a0 * b1;
  const std::uint64_t p10 = a1 * b0;
  const std::uint64_t p11 = a1 * b1;

  const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
  U128 out;
  out.lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
  out.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return out;
}

/// Long division of a 128-bit value by a 64-bit divisor. The quotient must fit in 64
/// bits; callers guarantee this by construction.
std::uint64_t div_u128_by_u64(U128 value, std::uint64_t divisor) noexcept {
  if (divisor == 0) {
    return 0;
  }
  std::uint64_t quotient = 0;
  std::uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t next = bit >= 64 ? ((value.hi >> (bit - 64)) & 1ull) : ((value.lo >> bit) & 1ull);
    const bool carry = (remainder >> 63) != 0;
    remainder = (remainder << 1) | next;
    if (carry || remainder >= divisor) {
      remainder -= divisor;
      if (bit >= 64) {
        // The quotient does not fit in 64 bits; the caller's precondition is violated.
        return UINT64_MAX;
      }
      quotient |= (1ull << bit);
    }
  }
  return quotient;
}

const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

/// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  d = doy - (153u * mp + 2u) / 5u + 1u;
  const int month = static_cast<int>(mp) + (mp < 10u ? 3 : -9);
  m = static_cast<unsigned>(month);
  y += (m <= 2u) ? 1 : 0;
}

std::string pad(std::uint64_t value, std::size_t width) {
  std::string text = std::to_string(value);
  if (text.size() < width) {
    text.insert(0, width - text.size(), '0');
  }
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::AlreadyExists: return "ALREADY_EXISTS";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::BoundExceeded: return "BOUND_EXCEEDED";
    case ErrorCode::CapacityInconsistent: return "CAPACITY_INCONSISTENT";
    case ErrorCode::StaleGeneration: return "STALE_GENERATION";
    case ErrorCode::StaleEpoch: return "STALE_EPOCH";
    case ErrorCode::StaleBoot: return "STALE_BOOT";
    case ErrorCode::FencedPublisher: return "FENCED_PUBLISHER";
    case ErrorCode::SequenceRegression: return "SEQUENCE_REGRESSION";
    case ErrorCode::SequenceGap: return "SEQUENCE_GAP";
    case ErrorCode::ProtocolViolation: return "PROTOCOL_VIOLATION";
    case ErrorCode::ConnectionClosed: return "CONNECTION_CLOSED";
    case ErrorCode::OversizedPayload: return "OVERSIZED_PAYLOAD";
    case ErrorCode::IntegrityFailure: return "INTEGRITY_FAILURE";
    case ErrorCode::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case ErrorCode::UnsupportedOperation: return "UNSUPPORTED_OPERATION";
    case ErrorCode::InvalidTransition: return "INVALID_TRANSITION";
    case ErrorCode::CorruptState: return "CORRUPT_STATE";
    case ErrorCode::NotReady: return "NOT_READY";
    case ErrorCode::Backpressure: return "BACKPRESSURE";
    case ErrorCode::ShuttingDown: return "SHUTTING_DOWN";
    case ErrorCode::Internal: return "INTERNAL";
  }
  return "UNKNOWN_ERROR";
}

std::string Error::to_string() const {
  std::string text(fo::to_string(code_));
  if (!message_.empty()) {
    text += ": ";
    text += message_;
  }
  if (!detail_.empty()) {
    text += " (";
    text += detail_;
    text += ")";
  }
  return text;
}

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

std::string_view version_string() noexcept { return FO_VERSION_STRING; }

Version version() noexcept { return Version{}; }

std::string_view build_banner() noexcept {
  return "Federation Observatory 1.0.0 [protocol 1, persistence 1, c++20]";
}

std::uint32_t persistence_format_version() noexcept { return 1; }
std::uint32_t protocol_version() noexcept { return 1; }

// ---------------------------------------------------------------------------
// Checked arithmetic and checksums
// ---------------------------------------------------------------------------

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > UINT64_MAX - b) {
    return false;
  }
  out = a + b;
  return true;
}

bool checked_sub(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > a) {
    return false;
  }
  out = a - b;
  return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > UINT64_MAX / b) {
    return false;
  }
  out = a * b;
  return true;
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  return checked_add(a, b, out) ? out : UINT64_MAX;
}

bool checked_sum(const std::uint64_t* values, std::size_t count, std::uint64_t& out) noexcept {
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!checked_add(total, values[i], total)) {
      return false;
    }
  }
  out = total;
  return true;
}

std::uint32_t crc32(const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  const auto& table = crc_table();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(std::string_view text) noexcept {
  return crc32(text.data(), text.size());
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint32_t basis_points_of(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0) {
    return 0;
  }
  const U128 scaled = mul_u64(part, 10000ull);
  const std::uint64_t quotient = div_u128_by_u64(scaled, whole);
  return quotient > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(quotient);
}

std::string percent_string(std::uint64_t part, std::uint64_t whole) {
  if (whole == 0) {
    return "0.00%";
  }
  // Percentage with four decimal places of internal precision, rounded half up to two.
  const U128 scaled = mul_u64(part, 1000000ull);
  const std::uint64_t micro = div_u128_by_u64(scaled, whole);
  if (micro == UINT64_MAX) {
    // The exact percentage does not fit in 64 bits. Say so rather than wrapping.
    return std::string(">1.8e14%");
  }
  std::uint64_t hundredths = (micro + 50) / 100;
  const std::uint64_t whole_part = hundredths / 100;
  hundredths %= 100;
  return std::to_string(whole_part) + "." + pad(hundredths, 2) + "%";
}

std::uint64_t DeterministicRandom::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::uint32_t DeterministicRandom::below(std::uint32_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(next_u64() % bound);
}

bool DeterministicRandom::chance(std::uint32_t numerator, std::uint32_t denominator) noexcept {
  if (denominator == 0) {
    return false;
  }
  return below(denominator) < numerator;
}

// ---------------------------------------------------------------------------
// Encoder / Decoder
// ---------------------------------------------------------------------------

void Encoder::u8(std::uint8_t v) { buffer_.push_back(v); }

void Encoder::u16(std::uint16_t v) {
  buffer_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void Encoder::u32(std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    buffer_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

void Encoder::u64(std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buffer_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

void Encoder::i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

void Encoder::f64(double v) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v), "double must be 64 bits");
  std::memcpy(&bits, &v, sizeof(bits));
  u64(bits);
}

void Encoder::boolean(bool v) { u8(v ? 1u : 0u); }

void Encoder::raw(const void* data, std::size_t length) {
  if (failed_) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + length);
}

void Encoder::str(std::string_view v) {
  if (failed_) {
    return;
  }
  if (v.size() > kMaxStringFieldBytes) {
    fail(ErrorCode::BoundExceeded, "string field exceeds the maximum encoded length");
    return;
  }
  u32(static_cast<std::uint32_t>(v.size()));
  raw(v.data(), v.size());
}

void Encoder::text_block(std::string_view v) {
  if (failed_) {
    return;
  }
  if (v.size() > kMaxFrameBytes) {
    fail(ErrorCode::OversizedPayload, "encoded text block exceeds the frame bound");
    return;
  }
  u32(static_cast<std::uint32_t>(v.size()));
  raw(v.data(), v.size());
}

void Encoder::fail(ErrorCode code, std::string message) {
  if (!failed_) {
    failed_ = true;
    error_ = Error(code, std::move(message));
  }
}

Decoder::Decoder(const std::uint8_t* data, std::size_t length, std::size_t max_bytes)
    : data_(data), length_(length), max_bytes_(max_bytes) {
  if (length_ > max_bytes_) {
    length_ = max_bytes_;
    over_budget_ = true;
  }
}

Status Decoder::need(std::size_t n) const {
  if (over_budget_) {
    return fail(ErrorCode::OversizedPayload, "decoder input exceeds the configured budget");
  }
  if (n > length_ - offset_) {
    return fail(ErrorCode::ProtocolViolation, "decoder input truncated");
  }
  return Status::success();
}

Result<std::uint8_t> Decoder::u8() {
  const Status s = need(1);
  if (!s.ok()) {
    return s.error();
  }
  return data_[offset_++];
}

Result<std::uint16_t> Decoder::u16() {
  const Status s = need(2);
  if (!s.ok()) {
    return s.error();
  }
  std::uint16_t v = static_cast<std::uint16_t>(data_[offset_]) |
                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
  offset_ += 2;
  return v;
}

Result<std::uint32_t> Decoder::u32() {
  const Status s = need(4);
  if (!s.ok()) {
    return s.error();
  }
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  offset_ += 4;
  return v;
}

Result<std::uint64_t> Decoder::u64() {
  const Status s = need(8);
  if (!s.ok()) {
    return s.error();
  }
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  offset_ += 8;
  return v;
}

Result<std::int64_t> Decoder::i64() {
  const Result<std::uint64_t> raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::int64_t>(raw.value());
}

Result<double> Decoder::f64() {
  const Result<std::uint64_t> raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  const std::uint64_t bits = raw.value();
  double v = 0.0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

Result<bool> Decoder::boolean() {
  const Result<std::uint8_t> raw = u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > 1u) {
    return Error(ErrorCode::ProtocolViolation, "boolean field is not 0 or 1");
  }
  return raw.value() == 1u;
}

Result<std::string> Decoder::str() {
  const Result<std::uint32_t> length = u32();
  if (!length.ok()) {
    return length.error();
  }
  const std::size_t n = length.value();
  if (n > kMaxStringFieldBytes) {
    return Error(ErrorCode::BoundExceeded, "encoded string field exceeds the maximum length");
  }
  const Status s = need(n);
  if (!s.ok()) {
    return s.error();
  }
  std::string value(reinterpret_cast<const char*>(data_ + offset_), n);
  offset_ += n;
  return value;
}

Result<std::string> Decoder::text_block() {
  const Result<std::uint32_t> length = u32();
  if (!length.ok()) {
    return length.error();
  }
  const std::size_t n = length.value();
  if (n > kMaxFrameBytes) {
    return Error(ErrorCode::OversizedPayload, "encoded text block exceeds the frame bound");
  }
  const Status s = need(n);
  if (!s.ok()) {
    return s.error();
  }
  std::string value(reinterpret_cast<const char*>(data_ + offset_), n);
  offset_ += n;
  return value;
}

Status Decoder::raw(void* out, std::size_t length) {
  const Status s = need(length);
  if (!s.ok()) {
    return s;
  }
  std::memcpy(out, data_ + offset_, length);
  offset_ += length;
  return Status::success();
}

Status Decoder::skip(std::size_t length) {
  const Status s = need(length);
  if (!s.ok()) {
    return s;
  }
  offset_ += length;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentifierLength) {
    return false;
  }
  for (const char c : text) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    const bool punctuation = c == '.' || c == '_' || c == '-' || c == ':' || c == '@' || c == '/' ||
                             c == '+' || c == '=';
    if (!alnum && !punctuation) {
      return false;
    }
  }
  return true;
}

TimestampNanos now_unix_nanos() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string format_timestamp(TimestampNanos nanos) {
  if (nanos == 0) {
    return "unset";
  }
  std::int64_t seconds = nanos / 1000000000;
  std::int64_t fraction = nanos % 1000000000;
  if (fraction < 0) {
    fraction += 1000000000;
    seconds -= 1;
  }
  std::int64_t days = seconds / 86400;
  std::int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  std::int64_t year = 0;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);
  const std::uint64_t hours = static_cast<std::uint64_t>(rem / 3600);
  const std::uint64_t minutes = static_cast<std::uint64_t>((rem % 3600) / 60);
  const std::uint64_t secs = static_cast<std::uint64_t>(rem % 60);
  const std::uint64_t millis = static_cast<std::uint64_t>(fraction / 1000000);
  std::string out;
  out.reserve(24);
  out += pad(static_cast<std::uint64_t>(year), 4);
  out += '-';
  out += pad(month, 2);
  out += '-';
  out += pad(day, 2);
  out += 'T';
  out += pad(hours, 2);
  out += ':';
  out += pad(minutes, 2);
  out += ':';
  out += pad(secs, 2);
  out += '.';
  out += pad(millis, 3);
  out += 'Z';
  return out;
}

std::string render_id(std::string_view id) { return id.empty() ? std::string("<none>") : std::string(id); }

std::string_view render_unset_generation() noexcept { return "<unset>"; }

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

Status check_bound_range(std::size_t value, std::size_t low, std::size_t high, const char* name) {
  if (value < low || value > high) {
    return fail(ErrorCode::InvalidArgument, "bound out of range", name);
  }
  return Status::success();
}

Status check_bound_nonzero(std::size_t value, const char* name) {
  if (value == 0) {
    return fail(ErrorCode::InvalidArgument, "bound must be non-zero", name);
  }
  return Status::success();
}

Status Bounds::validate() const {
  struct RangeCheck {
    std::size_t value;
    std::size_t low;
    std::size_t high;
    const char* name;
  };
  const RangeCheck ranges[] = {
      {max_frame_bytes, kMinFrameBytes, kMaxFrameBytes, "max_frame_bytes"},
      {max_metadata_bytes, 256, kMaxMetadataBytes, "max_metadata_bytes"},
      {max_query_response_bytes, 1024, kMaxQueryResponseBytes, "max_query_response_bytes"},
      {max_snapshot_bytes, 1024, kMaxSnapshotBytes, "max_snapshot_bytes"},
      {max_persistence_bytes, 1024, kMaxPersistenceBytes, "max_persistence_bytes"},
      {max_evidence_per_finding, kMinEvidencePerFinding, kMaxEvidencePerFinding,
       "max_evidence_per_finding"},
      {max_threads, 1, 1024, "max_threads"},
      {max_connections, 1, 65536, "max_connections"},
      {max_ingest_queue_depth, 1, 1u << 24, "max_ingest_queue_depth"},
      {max_publishers, 1, 1u << 20, "max_publishers"},
      {max_connections_per_publisher, 1, 1024, "max_connections_per_publisher"},
      {max_outstanding_requests_per_connection, 1, 65536,
       "max_outstanding_requests_per_connection"},
  };
  for (const RangeCheck& check : ranges) {
    const Status s = check_bound_range(check.value, check.low, check.high, check.name);
    if (!s.ok()) {
      return s;
    }
  }

  struct NonZeroCheck {
    std::size_t value;
    const char* name;
  };
  const NonZeroCheck non_zero[] = {
      {max_federations, "max_federations"},
      {max_sites, "max_sites"},
      {max_clusters, "max_clusters"},
      {max_accelerator_classes, "max_accelerator_classes"},
      {max_runtimes, "max_runtimes"},
      {max_backends, "max_backends"},
      {max_artifacts, "max_artifacts"},
      {max_workloads, "max_workloads"},
      {max_workload_classes, "max_workload_classes"},
      {max_capability_entries, "max_capability_entries"},
      {max_capacity_records, "max_capacity_records"},
      {max_placement_history, "max_placement_history"},
      {max_migration_history, "max_migration_history"},
      {max_portability_records, "max_portability_records"},
      {max_publishers_per_federation, "max_publishers_per_federation"},
      {max_domains, "max_domains"},
      {max_policies, "max_policies"},
      {max_aggregate_findings, "max_aggregate_findings"},
      {max_history_per_federation, "max_history_per_federation"},
  };
  for (const NonZeroCheck& check : non_zero) {
    const Status s = check_bound_nonzero(check.value, check.name);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::success();
}

const Bounds& default_bounds() noexcept {
  static const Bounds bounds{};
  return bounds;
}

std::string render_bounds(const Bounds& bounds) {
  std::vector<std::vector<std::string>> rows;
  const auto row = [&rows](const char* name, std::size_t value) {
    rows.push_back({name, std::to_string(value)});
  };
  row("max_federations", bounds.max_federations);
  row("max_sites", bounds.max_sites);
  row("max_clusters", bounds.max_clusters);
  row("max_accelerator_classes", bounds.max_accelerator_classes);
  row("max_runtimes", bounds.max_runtimes);
  row("max_backends", bounds.max_backends);
  row("max_artifacts", bounds.max_artifacts);
  row("max_workloads", bounds.max_workloads);
  row("max_workload_classes", bounds.max_workload_classes);
  row("max_capability_entries", bounds.max_capability_entries);
  row("max_capacity_records", bounds.max_capacity_records);
  row("max_placement_history", bounds.max_placement_history);
  row("max_migration_history", bounds.max_migration_history);
  row("max_portability_records", bounds.max_portability_records);
  row("max_publishers", bounds.max_publishers);
  row("max_publishers_per_federation", bounds.max_publishers_per_federation);
  row("max_domains", bounds.max_domains);
  row("max_policies", bounds.max_policies);
  row("max_aggregate_findings", bounds.max_aggregate_findings);
  row("max_metadata_bytes", bounds.max_metadata_bytes);
  row("max_frame_bytes", bounds.max_frame_bytes);
  row("max_query_response_bytes", bounds.max_query_response_bytes);
  row("max_snapshot_bytes", bounds.max_snapshot_bytes);
  row("max_persistence_bytes", bounds.max_persistence_bytes);
  row("max_evidence_per_finding", bounds.max_evidence_per_finding);
  row("max_threads", bounds.max_threads);
  row("max_connections", bounds.max_connections);
  row("max_ingest_queue_depth", bounds.max_ingest_queue_depth);
  row("max_connections_per_publisher", bounds.max_connections_per_publisher);
  row("max_outstanding_requests_per_connection", bounds.max_outstanding_requests_per_connection);
  row("max_history_per_federation", bounds.max_history_per_federation);
  return render_table({"bound", "value"}, rows, "");
}

// ---------------------------------------------------------------------------
// Evidence classification
// ---------------------------------------------------------------------------

std::string_view to_string(Tri value) noexcept {
  switch (value) {
    case Tri::No: return "NO";
    case Tri::Unknown: return "UNKNOWN";
    case Tri::Yes: return "YES";
  }
  return "UNKNOWN";
}

Tri tri_and(Tri a, Tri b) noexcept {
  if (a == Tri::No || b == Tri::No) return Tri::No;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::Yes;
}

Tri tri_or(Tri a, Tri b) noexcept {
  if (a == Tri::Yes || b == Tri::Yes) return Tri::Yes;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::No;
}

Tri tri_not(Tri a) noexcept {
  switch (a) {
    case Tri::No: return Tri::Yes;
    case Tri::Yes: return Tri::No;
    case Tri::Unknown: return Tri::Unknown;
  }
  return Tri::Unknown;
}

Tri tri_from_bool(bool value) noexcept { return value ? Tri::Yes : Tri::No; }

std::string_view to_string(Precision precision) noexcept {
  switch (precision) {
    case Precision::Exact: return "EXACT";
    case Precision::Aggregated: return "AGGREGATED";
    case Precision::Sampled: return "SAMPLED";
    case Precision::Derived: return "DERIVED";
    case Precision::Inferred: return "INFERRED";
    case Precision::Ambiguous: return "AMBIGUOUS";
    case Precision::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

int precision_rank(Precision precision) noexcept { return static_cast<int>(precision); }

Precision weakest(Precision a, Precision b) noexcept {
  return precision_rank(a) >= precision_rank(b) ? a : b;
}

Precision strongest(Precision a, Precision b) noexcept {
  return precision_rank(a) <= precision_rank(b) ? a : b;
}

bool parse_precision(std::string_view text, Precision& out) noexcept {
  struct Entry { std::string_view name; Precision value; };
  static constexpr Entry kEntries[] = {
      {"EXACT", Precision::Exact},         {"AGGREGATED", Precision::Aggregated},
      {"SAMPLED", Precision::Sampled},     {"DERIVED", Precision::Derived},
      {"INFERRED", Precision::Inferred},   {"AMBIGUOUS", Precision::Ambiguous},
      {"UNKNOWN", Precision::Unknown},
  };
  for (const Entry& entry : kEntries) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ReasonBasis basis) noexcept {
  switch (basis) {
    case ReasonBasis::Observed: return "OBSERVED";
    case ReasonBasis::Derived: return "DERIVED";
    case ReasonBasis::Inferred: return "INFERRED";
    case ReasonBasis::Unattributed: return "UNATTRIBUTED";
  }
  return "UNATTRIBUTED";
}

int reason_basis_rank(ReasonBasis basis) noexcept { return static_cast<int>(basis); }

ReasonBasis weakest(ReasonBasis a, ReasonBasis b) noexcept {
  return reason_basis_rank(a) >= reason_basis_rank(b) ? a : b;
}

std::string_view to_string(EvidenceClass cls) noexcept {
  switch (cls) {
    case EvidenceClass::Real: return "REAL";
    case EvidenceClass::Synthetic: return "SYNTHETIC";
    case EvidenceClass::Unsupported: return "UNSUPPORTED";
    case EvidenceClass::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

int evidence_class_rank(EvidenceClass cls) noexcept { return static_cast<int>(cls); }

EvidenceClass weaker(EvidenceClass a, EvidenceClass b) noexcept {
  return evidence_class_rank(a) >= evidence_class_rank(b) ? a : b;
}

bool parse_evidence_class(std::string_view text, EvidenceClass& out) noexcept {
  if (text == "REAL") { out = EvidenceClass::Real; return true; }
  if (text == "SYNTHETIC") { out = EvidenceClass::Synthetic; return true; }
  if (text == "UNSUPPORTED") { out = EvidenceClass::Unsupported; return true; }
  if (text == "UNKNOWN") { out = EvidenceClass::Unknown; return true; }
  return false;
}

std::string_view to_string(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::HostProbe: return "HOST_PROBE";
    case Provenance::ToolchainProbe: return "TOOLCHAIN_PROBE";
    case Provenance::ArtifactInspection: return "ARTIFACT_INSPECTION";
    case Provenance::FederationController: return "FEDERATION_CONTROLLER";
    case Provenance::ClusterController: return "CLUSTER_CONTROLLER";
    case Provenance::Scheduler: return "SCHEDULER";
    case Provenance::RuntimeRegistry: return "RUNTIME_REGISTRY";
    case Provenance::HardwareCapabilityRegistry: return "HARDWARE_CAPABILITY_REGISTRY";
    case Provenance::ArtifactRegistry: return "ARTIFACT_REGISTRY";
    case Provenance::MigrationRuntime: return "MIGRATION_RUNTIME";
    case Provenance::ResourceBroker: return "RESOURCE_BROKER";
    case Provenance::ExternalInventory: return "EXTERNAL_INVENTORY";
    case Provenance::SyntheticBackend: return "SYNTHETIC_BACKEND";
    case Provenance::ImportedTrace: return "IMPORTED_TRACE";
    case Provenance::DerivedAnalysis: return "DERIVED_ANALYSIS";
    case Provenance::PublisherReport: return "PUBLISHER_REPORT";
    case Provenance::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool parse_provenance(std::string_view text, Provenance& out) noexcept {
  for (int i = 0; i <= static_cast<int>(Provenance::Unknown); ++i) {
    const auto candidate = static_cast<Provenance>(i);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool operator==(const EvidenceRef& a, const EvidenceRef& b) noexcept {
  return a.source == b.source && a.evidence_class == b.evidence_class && a.source_id == b.source_id &&
         a.generation == b.generation && a.precision == b.precision && a.detail == b.detail;
}

bool operator<(const EvidenceRef& a, const EvidenceRef& b) noexcept {
  if (a.source != b.source) return a.source < b.source;
  if (a.source_id != b.source_id) return a.source_id < b.source_id;
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.evidence_class != b.evidence_class) return a.evidence_class < b.evidence_class;
  if (a.precision != b.precision) return a.precision < b.precision;
  return a.detail < b.detail;
}

std::string EvidenceRef::to_string() const {
  std::string text(fo::to_string(source));
  text += '/';
  text += fo::to_string(evidence_class);
  text += '/';
  text += fo::to_string(precision);
  if (!source_id.empty()) {
    text += " src=";
    text += source_id;
  }
  if (generation.is_set()) {
    text += " gen=";
    text += generation.to_string();
  }
  if (!detail.empty()) {
    text += " - ";
    text += detail;
  }
  return text;
}

Status EvidenceList::add(EvidenceRef ref) {
  ref.source_id = truncate_with_marker(ref.source_id, kMaxIdentifierLength);
  ref.detail = truncate_with_marker(ref.detail, 512);
  const auto existing = std::find(items_.begin(), items_.end(), ref);
  if (existing != items_.end()) {
    return Status::success();
  }
  if (items_.size() >= limit_) {
    return fail(ErrorCode::BoundExceeded, "evidence list is full",
                std::to_string(limit_) + " references retained");
  }
  items_.push_back(std::move(ref));
  std::stable_sort(items_.begin(), items_.end());
  return Status::success();
}

Status EvidenceList::add(Provenance source, EvidenceClass cls, std::string source_id,
                         Precision precision, std::string detail) {
  EvidenceRef ref;
  ref.source = source;
  ref.evidence_class = cls;
  ref.source_id = std::move(source_id);
  ref.precision = precision;
  ref.detail = std::move(detail);
  return add(std::move(ref));
}

Precision EvidenceList::combined_precision() const noexcept {
  Precision result = Precision::Unknown;
  bool first = true;
  for (const EvidenceRef& ref : items_) {
    if (first) {
      result = ref.precision;
      first = false;
    } else {
      result = weakest(result, ref.precision);
    }
  }
  return result;
}

EvidenceClass EvidenceList::combined_class() const noexcept {
  EvidenceClass result = EvidenceClass::Unknown;
  bool first = true;
  for (const EvidenceRef& ref : items_) {
    if (first) {
      result = ref.evidence_class;
      first = false;
    } else {
      result = weaker(result, ref.evidence_class);
    }
  }
  return result;
}

bool EvidenceList::all_real() const noexcept {
  if (items_.empty()) {
    return false;
  }
  for (const EvidenceRef& ref : items_) {
    if (ref.evidence_class != EvidenceClass::Real) {
      return false;
    }
  }
  return true;
}

bool EvidenceList::any_synthetic() const noexcept {
  for (const EvidenceRef& ref : items_) {
    if (ref.evidence_class == EvidenceClass::Synthetic) {
      return true;
    }
  }
  return false;
}

std::string EvidenceList::render(std::string_view indent) const {
  if (items_.empty()) {
    return std::string(indent) + "evidence: <none>";
  }
  std::string out(indent);
  out += "evidence:";
  for (const EvidenceRef& ref : items_) {
    out += '\n';
    out += indent;
    out += "  - ";
    out += ref.to_string();
  }
  return out;
}

std::string evidence_digest(const EvidenceList& evidence) {
  std::string canonical;
  for (const EvidenceRef& ref : evidence.items()) {
    canonical += fo::to_string(ref.source);
    canonical += '|';
    canonical += fo::to_string(ref.evidence_class);
    canonical += '|';
    canonical += fo::to_string(ref.precision);
    canonical += '|';
    canonical += ref.source_id;
    canonical += '|';
    canonical += ref.generation.to_string();
    canonical += '|';
    canonical += ref.detail;
    canonical += '\n';
  }
  return hex_digest64(fnv1a64(canonical));
}

// ---------------------------------------------------------------------------
// Deterministic rendering helpers
// ---------------------------------------------------------------------------

std::string indent_block(std::string_view text, std::string_view indent) {
  std::string out;
  out.reserve(text.size() + indent.size() * 8);
  std::size_t start = 0;
  bool first = true;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    if (!first) {
      out += '\n';
    }
    first = false;
    if (!line.empty()) {
      out += indent;
      out += line;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

std::string render_table(const std::vector<std::string>& headers,
                         const std::vector<std::vector<std::string>>& rows, std::string_view indent) {
  std::vector<std::size_t> widths(headers.size(), 0);
  for (std::size_t i = 0; i < headers.size(); ++i) {
    widths[i] = headers[i].size();
  }
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  const auto emit_row = [&](const std::vector<std::string>& row) {
    std::string line(indent);
    for (std::size_t i = 0; i < widths.size(); ++i) {
      const std::string cell = i < row.size() ? row[i] : std::string();
      line += cell;
      if (i + 1 < widths.size()) {
        line.append(widths[i] - cell.size() + 2, ' ');
      }
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    return line;
  };

  std::string out = emit_row(headers);
  std::string separator(indent);
  for (std::size_t i = 0; i < widths.size(); ++i) {
    separator.append(widths[i], '-');
    if (i + 1 < widths.size()) {
      separator.append(2, ' ');
    }
  }
  out += '\n';
  out += separator;
  for (const auto& row : rows) {
    out += '\n';
    out += emit_row(row);
  }
  return out;
}

std::string render_kv(std::string_view key, std::string_view value, std::string_view indent) {
  std::string out(indent);
  out += key;
  out += ": ";
  out += value;
  return out;
}

std::string hex_digest64(std::uint64_t value) {
  static const char* kHex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[value & 0xFu];
    value >>= 4;
  }
  return out;
}

std::string digest_text(std::string_view text) { return hex_digest64(fnv1a64(text)); }

std::string join_strings(const std::vector<std::string>& values, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += separator;
    }
    out += values[i];
  }
  return out;
}

std::string truncate_with_marker(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  if (max_bytes <= 3) {
    return std::string(text.substr(0, max_bytes));
  }
  std::string out(text.substr(0, max_bytes - 3));
  out += "...";
  return out;
}

}  // namespace fo
