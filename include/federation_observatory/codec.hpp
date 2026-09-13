// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"

namespace fo {

/// Unsigned 64-bit quantity of bytes. All capacity arithmetic in the runtime is
/// performed on this type using the checked helpers below; silent wrap-around is a
/// defect, not a tolerated condition.
using Bytes = std::uint64_t;

/// Checked summation. Returns false and leaves p out untouched on overflow.
[[nodiscard]] FO_API bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

/// Checked subtraction. Returns false when p b > \p a.
[[nodiscard]] FO_API bool checked_sub(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

/// Checked product.
[[nodiscard]] FO_API bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

/// Saturating sum, used only where an exact value is known to be unrepresentable
/// because a component was already saturated from UNKNOWN evidence.
[[nodiscard]] FO_API std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept;

/// Sum a sequence with checked arithmetic.
[[nodiscard]] FO_API bool checked_sum(const std::uint64_t* values, std::size_t count,
                                      std::uint64_t& out) noexcept;

/// 32-bit CRC (IEEE 802.3 polynomial, reflected) over a byte range.
[[nodiscard]] FO_API std::uint32_t crc32(const void* data, std::size_t length) noexcept;
[[nodiscard]] FO_API std::uint32_t crc32(std::string_view text) noexcept;

/// FNV-1a 64-bit hash, used for deterministic sharding and stable ordering only.
[[nodiscard]] FO_API std::uint64_t fnv1a64(std::string_view text) noexcept;

/// Percentage of \p part within \p whole in basis points (hundredths of a percent),
/// computed exactly with 128-bit intermediates and saturated only on overflow of the
/// 32-bit result.
[[nodiscard]] FO_API std::uint32_t basis_points_of(std::uint64_t part, std::uint64_t whole) noexcept;

/// Exact fixed-point ratio rendered as a decimal percentage with two fractional
/// digits, e.g. "28.13%". No floating point is used in any accounting path.
[[nodiscard]] FO_API std::string percent_string(std::uint64_t part, std::uint64_t whole);

/// Deterministic pseudo-random generator (SplitMix64). Used by the synthetic
/// federation backend and by the seeded property tests. It is reproducible across
/// platforms and standard libraries by construction; nothing in the runtime depends on
/// <random>.
class FO_API DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) : state_(seed), seed_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  /// Uniform value in [0, bound). Returns 0 when bound == 0.
  [[nodiscard]] std::uint32_t below(std::uint32_t bound) noexcept;
  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) noexcept;
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t seed_ = 0;
};

/// Deterministic, allocation-bounded binary writer. All multi-byte integers are
/// little-endian and every length-prefixed field is validated against \p max_bytes.
class FO_API Encoder {
 public:
  explicit Encoder(std::size_t reserve = 256) { buffer_.reserve(reserve); }

  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void i64(std::int64_t v);
  void f64(double v);
  void boolean(bool v);
  void raw(const void* data, std::size_t length);
  /// Length-prefixed bounded string. Fails the encoder when the value exceeds
  /// kMaxStringFieldBytes instead of truncating it.
  void str(std::string_view v);

  /// Length-prefixed block for text that is legitimately large, such as a deterministic
  /// rendering. Bounded by kMaxFrameBytes rather than kMaxStringFieldBytes; the enclosing
  /// frame or file bound still applies.
  void text_block(std::string_view v);

  /// Record an encoding failure. An encoder that has failed produces no usable output;
  /// the top-level encoder returns the recorded error instead of a buffer.
  void fail(ErrorCode code, std::string message);
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  /// Move the encoded bytes out and leave the encoder empty.
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::uint8_t> buffer_;
  bool failed_ = false;
  Error error_;
};

/// Deterministic binary reader. Every read is bounds-checked; a short or malformed
/// buffer produces a classified Error rather than undefined behaviour.
class FO_API Decoder {
 public:
  Decoder(const std::uint8_t* data, std::size_t length, std::size_t max_bytes);

  [[nodiscard]] const std::uint8_t* cursor() const noexcept { return data_ + offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return length_ - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == length_; }

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<double> f64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::string> str();
  [[nodiscard]] Result<std::string> text_block();
  [[nodiscard]] Status raw(void* out, std::size_t length);
  [[nodiscard]] Status skip(std::size_t length);

 private:
  [[nodiscard]] Status need(std::size_t n) const;

  const std::uint8_t* data_ = nullptr;
  std::size_t length_ = 0;
  std::size_t offset_ = 0;
  std::size_t max_bytes_ = 0;
  bool over_budget_ = false;
};

}  // namespace fo
