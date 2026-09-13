// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "federation_observatory/bounds.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/protocol.hpp"

namespace fo {

struct FO_API CoordinatorConfig {
  std::string bind_host = "127.0.0.1";
  /// 0 requests an ephemeral port; the assigned port is available from port().
  std::uint16_t port = 0;
  Bounds bounds;
  /// Optional durable state path. When set, start() restores it and stop() saves it.
  std::string state_path;
  /// Optional readiness file: written after the listener is bound, containing the
  /// bound port. Used by the multiprocess proofs and by operators.
  std::string ready_file;
  /// Threads that apply publications. One thread is the default so that application
  /// order equals queue order and explanations are bit-for-bit reproducible. Raising it
  /// preserves per-key determinism (each key is fence-checked under the model lock) but
  /// not global ordering.
  std::size_t worker_threads = 1;
  std::size_t accept_threads = 1;
  /// Server-side poll quantum for accept and read loops. Not a request timeout: a
  /// connection that is idle stays open indefinitely.
  int poll_millis = 50;
  /// Permit FENCE messages. When false every FENCE is refused; a coordinator with no
  /// administrative channel must not accept remote authority revocation.
  bool allow_fence = true;
  /// Optional shared secret required by FENCE.
  std::string admin_token;
  /// Hard cap on concurrent connections; further accepts are closed immediately and
  /// counted.
  std::size_t max_connections = 256;
  /// Optional append-only log of accepted/rejected frames.
  std::string log_path;

  [[nodiscard]] Status validate() const;
};

/// The coordinator: one operating-system process that owns the authoritative
/// observation state and serves publishers and inspectors over framed TCP.
class FO_API FederationCoordinator {
 public:
  explicit FederationCoordinator(CoordinatorConfig config = {});
  ~FederationCoordinator();

  FederationCoordinator(const FederationCoordinator&) = delete;
  FederationCoordinator& operator=(const FederationCoordinator&) = delete;
  FederationCoordinator(FederationCoordinator&&) = delete;
  FederationCoordinator& operator=(FederationCoordinator&&) = delete;

  /// Bind, restore durable state when configured, and start serving. Idempotent.
  [[nodiscard]] Status start();
  /// Stop accepting, drain or classify queued work, close every socket, join every
  /// thread, and persist state when configured. Idempotent and self-join safe.
  [[nodiscard]] Status stop();
  [[nodiscard]] bool running() const noexcept;
  /// True once a client has sent a SHUTDOWN request. The hosting process is responsible
  /// for calling stop() when it observes this; the coordinator never stops itself, because
  /// only the process that owns the durable state knows when it is safe to write it.
  [[nodiscard]] bool shutdown_requested() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const std::string& bind_host() const noexcept;

  [[nodiscard]] FederationObservatory& observatory();
  [[nodiscard]] const FederationObservatory& observatory() const;

  [[nodiscard]] Status save_state(const std::string& path);
  [[nodiscard]] Status load_state(const std::string& path);

  /// Handle one frame as if it had arrived over the socket. Used by the deterministic
  /// protocol tests and by the in-process control path.
  [[nodiscard]] Result<protocol::Frame> handle_frame(const protocol::Frame& frame,
                                                     const std::string& peer);

  struct Counters {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t connections_closed_by_shutdown = 0;
    std::uint64_t active_connections = 0;
  };
  [[nodiscard]] Counters counters() const;
  [[nodiscard]] std::string stats_report() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Entry point used by the fo-coordinator executable.
FO_API int run_coordinator_main(int argc, char** argv);

}  // namespace fo
