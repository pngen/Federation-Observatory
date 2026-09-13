// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"

namespace fo::net {

/// Process-wide socket subsystem initialization. Idempotent and thread safe.
[[nodiscard]] FO_API Status initialize_sockets();
[[nodiscard]] FO_API Status shutdown_sockets();

/// Owns one connected TCP socket. Move-only; the destructor closes the handle.
class FO_API Socket {
 public:
  Socket() = default;
  explicit Socket(std::intptr_t handle) : handle_(handle) {}
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::intptr_t handle() const noexcept { return handle_; }
  void close() noexcept;
  [[nodiscard]] Status shutdown_send() noexcept;
  [[nodiscard]] Status set_nodelay(bool enabled) noexcept;
  [[nodiscard]] Status set_keepalive(bool enabled) noexcept;
  [[nodiscard]] std::string peer_address() const;

  /// Read up to \p length bytes.
  ///  * ok, n > 0  - n bytes were read;
  ///  * ok, n == 0 - no data became available within \p poll_millis; retry;
  ///  * error      - the connection is closed or unusable.
  ///
  /// \p poll_millis is a *server-side scheduling quantum*, not a request timeout: a
  /// runtime that blocked forever in recv() could not be shut down without leaking a
  /// thread or terminating a blocked read.
  [[nodiscard]] Result<std::size_t> read_some(void* buffer, std::size_t length,
                                              int poll_millis) noexcept;

  /// Write every byte or fail. Partial writes are retried; no framing is skipped.
  [[nodiscard]] Status write_all(const void* data, std::size_t length) noexcept;

 private:
  std::intptr_t handle_ = -1;
};

/// A bound TCP listener.
class FO_API TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  /// Bind \p host : \p port. Port 0 requests an ephemeral port; the assigned port is
  /// reported by port().
  [[nodiscard]] static Result<TcpListener> bind(const std::string& host, std::uint16_t port,
                                                std::size_t backlog);
  [[nodiscard]] Result<Socket> accept(int poll_millis) noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string endpoint() const;
  void close() noexcept;

 private:
  std::intptr_t handle_ = -1;
  std::uint16_t port_ = 0;
};

/// Connect to \p host : \p port.
[[nodiscard]] FO_API Result<Socket> connect(const std::string& host, std::uint16_t port);

/// Resolve \p host to a numeric IPv4 literal, or fail.
[[nodiscard]] FO_API Result<std::string> resolve_ipv4(const std::string& host);

}  // namespace fo::net
