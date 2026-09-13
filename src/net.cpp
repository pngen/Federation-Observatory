// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Real TCP sockets. Nothing in this file imitates a network: publishers and
// coordinators are separate operating-system processes communicating over framed TCP,
// and the publisher-death proof depends on that being literally true.

#include "federation_observatory/net.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fo::net {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;

int last_error() noexcept { return WSAGetLastError(); }

bool would_block(int error) noexcept { return error == WSAEWOULDBLOCK; }
bool interrupted(int error) noexcept { return error == WSAEINTR; }

std::string error_text(int error) {
  return "winsock error " + std::to_string(error);
}

void close_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::closesocket(socket);
  }
}

Status shut_native(NativeSocket socket) noexcept {
  if (::shutdown(socket, SD_SEND) == kSocketError) {
    const int error = last_error();
    if (error == WSAENOTCONN || error == WSAECONNRESET) {
      return Status::success();
    }
    return fail(ErrorCode::ProtocolViolation, "shutdown failed", error_text(error));
  }
  return Status::success();
}

void set_nonblocking(NativeSocket socket, bool enabled) noexcept {
  u_long mode = enabled ? 1ul : 0ul;
  ::ioctlsocket(socket, FIONBIO, &mode);
}

void set_reuse_address(NativeSocket socket) noexcept {
  BOOL value = TRUE;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&value),
               static_cast<int>(sizeof(value)));
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
constexpr int kSocketError = -1;

int last_error() noexcept { return errno; }

bool would_block(int error) noexcept { return error == EWOULDBLOCK || error == EAGAIN; }
bool interrupted(int error) noexcept { return error == EINTR; }

std::string error_text(int error) {
  return "errno " + std::to_string(error) + " (" + std::strerror(error) + ")";
}

void close_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::close(socket);
  }
}

Status shut_native(NativeSocket socket) noexcept {
  if (::shutdown(socket, SHUT_WR) != 0) {
    const int error = last_error();
    if (error == ENOTCONN || error == ECONNRESET) {
      return Status::success();
    }
    return fail(ErrorCode::ProtocolViolation, "shutdown failed", error_text(error));
  }
  return Status::success();
}

void set_nonblocking(NativeSocket socket, bool enabled) noexcept {
  const int flags = ::fcntl(socket, F_GETFL, 0);
  if (flags < 0) {
    return;
  }
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  ::fcntl(socket, F_SETFL, updated);
}

void set_reuse_address(NativeSocket socket) noexcept {
  int value = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &value, static_cast<socklen_t>(sizeof(value)));
}
#endif

std::atomic<int> g_socket_users{0};

constexpr std::size_t kMaxTransferChunk = 1u << 20;

timeval poll_interval(int poll_millis) noexcept {
  timeval value{};
  const int millis = poll_millis < 0 ? 0 : poll_millis;
  value.tv_sec = millis / 1000;
  value.tv_usec = static_cast<decltype(value.tv_usec)>((millis % 1000) * 1000);
  return value;
}

}  // namespace

Status initialize_sockets() {
#ifdef _WIN32
  if (g_socket_users.fetch_add(1) == 0) {
    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      g_socket_users.fetch_sub(1);
      return fail(ErrorCode::Internal, "WSAStartup failed", "winsock error " + std::to_string(result));
    }
  }
#else
  g_socket_users.fetch_add(1);
#endif
  return Status::success();
}

Status shutdown_sockets() {
#ifdef _WIN32
  if (g_socket_users.fetch_sub(1) == 1) {
    ::WSACleanup();
  }
#else
  g_socket_users.fetch_sub(1);
#endif
  return Status::success();
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != static_cast<std::intptr_t>(kInvalidSocket); }

void Socket::close() noexcept {
  if (valid()) {
    close_native(static_cast<NativeSocket>(handle_));
    handle_ = static_cast<std::intptr_t>(kInvalidSocket);
  }
}

Status Socket::shutdown_send() noexcept {
  if (!valid()) {
    return Status::success();
  }
  return shut_native(static_cast<NativeSocket>(handle_));
}

Status Socket::set_nodelay(bool enabled) noexcept {
  if (!valid()) {
    return fail(ErrorCode::InvalidArgument, "socket is not open");
  }
  const int value = enabled ? 1 : 0;
#ifdef _WIN32
  if (::setsockopt(static_cast<NativeSocket>(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value))) ==
      kSocketError) {
#else
  if (::setsockopt(static_cast<NativeSocket>(handle_), IPPROTO_TCP, TCP_NODELAY, &value,
                   static_cast<socklen_t>(sizeof(value))) == kSocketError) {
#endif
    return fail(ErrorCode::ProtocolViolation, "could not set TCP_NODELAY",
                error_text(last_error()));
  }
  return Status::success();
}

Status Socket::set_keepalive(bool enabled) noexcept {
  if (!valid()) {
    return fail(ErrorCode::InvalidArgument, "socket is not open");
  }
  const int value = enabled ? 1 : 0;
#ifdef _WIN32
  if (::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value))) ==
      kSocketError) {
#else
  if (::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_KEEPALIVE, &value,
                   static_cast<socklen_t>(sizeof(value))) == kSocketError) {
#endif
    return fail(ErrorCode::ProtocolViolation, "could not set SO_KEEPALIVE",
                error_text(last_error()));
  }
  return Status::success();
}

std::string Socket::peer_address() const {
  if (!valid()) {
    return "<closed>";
  }
  sockaddr_storage storage{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(storage));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(storage));
#endif
  if (::getpeername(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&storage),
                    &length) == kSocketError) {
    return "<unknown>";
  }
  char text[INET6_ADDRSTRLEN] = {0};
  std::uint16_t port = 0;
  if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    ::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    port = ntohs(address->sin_port);
  } else if (storage.ss_family == AF_INET6) {
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
    ::inet_ntop(AF_INET6, &address->sin6_addr, text, sizeof(text));
    port = ntohs(address->sin6_port);
  } else {
    return "<unknown>";
  }
  return std::string(text) + ":" + std::to_string(port);
}

Result<std::size_t> Socket::read_some(void* buffer, std::size_t length, int poll_millis) noexcept {
  if (!valid()) {
    return Error(ErrorCode::ConnectionClosed, "socket is not open");
  }
  if (length == 0) {
    return std::size_t{0};
  }
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);
  timeval interval = poll_interval(poll_millis);
#ifdef _WIN32
  const int selected = ::select(0, &read_set, nullptr, nullptr, &interval);
#else
  const int selected = ::select(socket + 1, &read_set, nullptr, nullptr, &interval);
#endif
  if (selected == 0) {
    return std::size_t{0};
  }
  if (selected == kSocketError) {
    const int error = last_error();
    if (interrupted(error)) {
      return std::size_t{0};
    }
    return Error(ErrorCode::ProtocolViolation, "select failed on read", error_text(error));
  }
  const std::size_t chunk = std::min<std::size_t>(length, kMaxTransferChunk);
  const int received = ::recv(socket, static_cast<char*>(buffer), static_cast<int>(chunk), 0);
  if (received == 0) {
    return Error(ErrorCode::ConnectionClosed, "peer closed the connection");
  }
  if (received < 0) {
    const int error = last_error();
    if (would_block(error) || interrupted(error)) {
      return std::size_t{0};
    }
    if (error == ECONNRESET
#ifdef _WIN32
        || error == WSAECONNRESET || error == WSAENOTSOCK
#endif
    ) {
      return Error(ErrorCode::ConnectionClosed, "connection reset by peer", error_text(error));
    }
    return Error(ErrorCode::ProtocolViolation, "recv failed", error_text(error));
  }
  return static_cast<std::size_t>(received);
}

Status Socket::write_all(const void* data, std::size_t length) noexcept {
  if (!valid()) {
    return fail(ErrorCode::ConnectionClosed, "socket is not open");
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  std::size_t offset = 0;
  while (offset < length) {
    const std::size_t chunk = std::min<std::size_t>(length - offset, kMaxTransferChunk);
    const int sent = ::send(socket, reinterpret_cast<const char*>(bytes + offset),
                            static_cast<int>(chunk), 0);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent == 0) {
      return fail(ErrorCode::ConnectionClosed, "send returned zero");
    }
    const int error = last_error();
    if (interrupted(error)) {
      continue;
    }
    if (would_block(error)) {
      fd_set write_set;
      FD_ZERO(&write_set);
      FD_SET(socket, &write_set);
      timeval interval = poll_interval(50);
#ifdef _WIN32
      const int selected = ::select(0, nullptr, &write_set, nullptr, &interval);
#else
      const int selected = ::select(socket + 1, nullptr, &write_set, nullptr, &interval);
#endif
      if (selected == kSocketError && !interrupted(last_error())) {
        return fail(ErrorCode::ConnectionClosed, "select failed on write",
                    error_text(last_error()));
      }
      continue;
    }
    return fail(ErrorCode::ConnectionClosed, "send failed", error_text(error));
  }
  return Status::success();
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
  other.handle_ = static_cast<std::intptr_t>(kInvalidSocket);
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = static_cast<std::intptr_t>(kInvalidSocket);
    other.port_ = 0;
  }
  return *this;
}

bool TcpListener::valid() const noexcept {
  return handle_ != static_cast<std::intptr_t>(kInvalidSocket);
}

void TcpListener::close() noexcept {
  if (valid()) {
    close_native(static_cast<NativeSocket>(handle_));
    handle_ = static_cast<std::intptr_t>(kInvalidSocket);
  }
  port_ = 0;
}

std::string TcpListener::endpoint() const {
  return "tcp://0.0.0.0:" + std::to_string(port_);
}

Result<TcpListener> TcpListener::bind(const std::string& host, std::uint16_t port,
                                      std::size_t backlog) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int resolved = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                                     &result);
  if (resolved != 0 || result == nullptr) {
    return Error(ErrorCode::InvalidArgument, "could not resolve bind host", host);
  }
  NativeSocket socket = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (socket == kInvalidSocket) {
    const std::string detail = error_text(last_error());
    ::freeaddrinfo(result);
    return Error(ErrorCode::Internal, "could not create listening socket", detail);
  }
  set_reuse_address(socket);
  if (::bind(socket, result->ai_addr, static_cast<int>(result->ai_addrlen)) == kSocketError) {
    const std::string detail = error_text(last_error());
    ::freeaddrinfo(result);
    close_native(socket);
    return Error(ErrorCode::InvalidArgument, "bind failed", host + ":" + service + " " + detail);
  }
  ::freeaddrinfo(result);
  if (::listen(socket, static_cast<int>(backlog == 0 ? 16 : backlog)) == kSocketError) {
    const std::string detail = error_text(last_error());
    close_native(socket);
    return Error(ErrorCode::Internal, "listen failed", detail);
  }

  sockaddr_storage local{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(local));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(local));
#endif
  TcpListener listener;
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
      local.ss_family == AF_INET) {
    listener.port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
  } else {
    listener.port_ = port;
  }
  listener.handle_ = static_cast<std::intptr_t>(socket);
  return listener;
}

Result<Socket> TcpListener::accept(int poll_millis) noexcept {
  if (!valid()) {
    return Error(ErrorCode::ConnectionClosed, "listener is not open");
  }
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);
  timeval interval = poll_interval(poll_millis);
#ifdef _WIN32
  const int selected = ::select(0, &read_set, nullptr, nullptr, &interval);
#else
  const int selected = ::select(socket + 1, &read_set, nullptr, nullptr, &interval);
#endif
  if (selected == 0) {
    // No pending connection within the scheduling quantum. This is not an error: the
    // caller loops and re-polls. It is what makes a graceful stop possible without
    // terminating a thread that is blocked inside a system call.
    return Socket{};
  }
  if (selected == kSocketError) {
    const int error = last_error();
    if (interrupted(error)) {
      return Socket{};
    }
    return Error(ErrorCode::ProtocolViolation, "select failed on listener", error_text(error));
  }
  sockaddr_storage remote{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(remote));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(remote));
#endif
  const NativeSocket accepted =
      ::accept(socket, reinterpret_cast<sockaddr*>(&remote), &length);
  if (accepted == kInvalidSocket) {
    const int error = last_error();
    if (would_block(error) || interrupted(error)) {
      return Socket{};
    }
    return Error(ErrorCode::ProtocolViolation, "accept failed", error_text(error));
  }
  return Socket(static_cast<std::intptr_t>(accepted));
}

Result<Socket> connect(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int resolved = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                                     &result);
  if (resolved != 0 || result == nullptr) {
    return Error(ErrorCode::InvalidArgument, "could not resolve peer host", host);
  }
  NativeSocket socket = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (socket == kInvalidSocket) {
    const std::string detail = error_text(last_error());
    ::freeaddrinfo(result);
    return Error(ErrorCode::Internal, "could not create socket", detail);
  }
  if (::connect(socket, result->ai_addr, static_cast<int>(result->ai_addrlen)) == kSocketError) {
    const std::string detail = error_text(last_error());
    ::freeaddrinfo(result);
    close_native(socket);
    return Error(ErrorCode::UnsupportedOperation, "connect failed",
                 host + ":" + service + " " + detail);
  }
  ::freeaddrinfo(result);
  return Socket(static_cast<std::intptr_t>(socket));
}

Result<std::string> resolve_ipv4(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int resolved = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (resolved != 0 || result == nullptr) {
    return Error(ErrorCode::InvalidArgument, "could not resolve host", host);
  }
  char text[INET_ADDRSTRLEN] = {0};
  const auto* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
  ::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
  ::freeaddrinfo(result);
  return std::string(text);
}

}  // namespace fo::net
