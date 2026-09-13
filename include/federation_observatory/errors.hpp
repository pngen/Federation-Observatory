// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "federation_observatory/export.hpp"

namespace fo {

/// Stable, classifiable failure codes. Every rejection path in the runtime names one
/// of these so that callers never have to parse message text.
enum class ErrorCode {
  Ok = 0,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  Conflict,
  BoundExceeded,
  CapacityInconsistent,
  StaleGeneration,
  StaleEpoch,
  StaleBoot,
  FencedPublisher,
  SequenceRegression,
  SequenceGap,
  ProtocolViolation,
  ConnectionClosed,
  OversizedPayload,
  IntegrityFailure,
  UnsupportedVersion,
  UnsupportedOperation,
  InvalidTransition,
  CorruptState,
  NotReady,
  Backpressure,
  ShuttingDown,
  Internal,
};

/// Human-readable, stable spelling of an error code.
[[nodiscard]] FO_API std::string_view to_string(ErrorCode code) noexcept;

/// A classified failure with a bounded, non-allocating-on-success description.
class FO_API Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message, std::string detail = {})
      : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// "code: message (detail)" - deterministic rendering used by CLI and logs.
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Error& a, const Error& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_ && a.detail_ == b.detail_;
  }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string detail_;
};

/// Outcome of an operation that has no value to return.
class FO_API Status {
 public:
  Status() = default;
  Status(const Error& e) : ok_(false), error_(e) {}

  [[nodiscard]] static Status success() { return Status(); }
  [[nodiscard]] static Status error(ErrorCode code, std::string message, std::string detail = {}) {
    return Status(Error(code, std::move(message), std::move(detail)));
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  [[nodiscard]] ErrorCode code() const noexcept { return ok_ ? ErrorCode::Ok : error_.code(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] std::string to_string() const { return ok_ ? std::string("ok") : error_.to_string(); }

 private:
  bool ok_ = true;
  Error error_;
};

/// Outcome of an operation that yields a value.
template <class T>
class Result {
 public:
  Result(T value) : ok_(true), value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(const Error& e) : ok_(false), error_(e) {}         // NOLINT(google-explicit-constructor)
  Result(const Status& s) : ok_(false), error_(s.error()) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  [[nodiscard]] const T& value() const noexcept { return value_; }
  [[nodiscard]] T& value() noexcept { return value_; }
  [[nodiscard]] T&& take() noexcept { return std::move(value_); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return ok_ ? ErrorCode::Ok : error_.code(); }

  [[nodiscard]] const T& value_or(const T& fallback) const noexcept { return ok_ ? value_ : fallback; }

 private:
  bool ok_ = false;
  T value_{};
  Error error_;
};

/// Convenience for propagating a failure without a value payload.
[[nodiscard]] inline Status fail(ErrorCode code, std::string message, std::string detail = {}) {
  return Status::error(code, std::move(message), std::move(detail));
}

}  // namespace fo
