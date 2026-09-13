// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Real operating-system child processes. The publisher-death and coordinator-restart
// proofs require an actual process to die, so this file never emulates one with a
// thread.

#include "federation_observatory/process.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fo {
namespace {

#ifdef _WIN32

/// Windows command-line quoting (the algorithm documented by Microsoft for
/// CreateProcess: backslashes are doubled before a quote, and embedded quotes are
/// escaped).
std::string quote_argument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string out = "\"";
  for (std::size_t i = 0; i < argument.size();) {
    std::size_t backslashes = 0;
    while (i < argument.size() && argument[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == argument.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (argument[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(argument[i]);
    }
    ++i;
  }
  out.push_back('"');
  return out;
}

#else

std::string quote_argument(const std::string& argument) { return argument; }

#endif

}  // namespace

ChildProcess::~ChildProcess() {
  if (valid() && !waited_ && !exited()) {
    // Never leak a running child: terminate and reap before releasing the handle.
    const Status killed = kill();
    (void)killed;
    const Result<int> reaped = wait();
    (void)reaped;
  }
  release();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), handle_(other.handle_), waited_(other.waited_), exit_code_(other.exit_code_) {
  other.pid_ = 0;
  other.handle_ = -1;
  other.waited_ = false;
  other.exit_code_ = -1;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (valid() && !waited_ && !exited()) {
      const Status killed = kill();
      (void)killed;
      const Result<int> reaped = wait();
      (void)reaped;
    }
    release();
    pid_ = other.pid_;
    handle_ = other.handle_;
    waited_ = other.waited_;
    exit_code_ = other.exit_code_;
    other.pid_ = 0;
    other.handle_ = -1;
    other.waited_ = false;
    other.exit_code_ = -1;
  }
  return *this;
}

void ChildProcess::release() noexcept {
#ifdef _WIN32
  if (handle_ != -1) {
    ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
  }
#else
  (void)0;
#endif
  handle_ = -1;
  pid_ = 0;
}

Result<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                         const std::string& workdir,
                                         const std::string& output_path) {
  if (argv.empty() || argv[0].empty()) {
    return Error(ErrorCode::InvalidArgument, "no executable was supplied to spawn");
  }

  std::string command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      command_line.push_back(' ');
    }
    command_line += quote_argument(argv[i]);
  }

#ifdef _WIN32
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  HANDLE output_file = INVALID_HANDLE_VALUE;
  BOOL inherit = FALSE;
  if (!output_path.empty()) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    output_file = ::CreateFileA(output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output_file == INVALID_HANDLE_VALUE) {
      return Error(ErrorCode::Internal, "could not create the child output file", output_path);
    }
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdOutput = output_file;
    startup.hStdError = output_file;
    inherit = TRUE;
  }
  const DWORD flags = CREATE_NO_WINDOW;
  if (::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, inherit, flags, nullptr,
                       workdir.empty() ? nullptr : workdir.c_str(), &startup, &process) == 0) {
    const DWORD error = ::GetLastError();
    if (output_file != INVALID_HANDLE_VALUE) {
      ::CloseHandle(output_file);
    }
    return Error(ErrorCode::NotFound, "could not start process",
                 argv[0] + " (windows error " + std::to_string(error) + ")");
  }
  if (output_file != INVALID_HANDLE_VALUE) {
    ::CloseHandle(output_file);
  }
  ::CloseHandle(process.hThread);
  ChildProcess child;
  child.pid_ = static_cast<std::uint32_t>(process.dwProcessId);
  child.handle_ = reinterpret_cast<std::intptr_t>(process.hProcess);
  return child;
#else
  const std::string executable = argv[0];
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    raw.push_back(const_cast<char*>(argument.c_str()));
  }
  raw.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return Error(ErrorCode::Internal, "fork failed");
  }
  if (pid == 0) {
    if (!workdir.empty()) {
      if (::chdir(workdir.c_str()) != 0) {
        ::_exit(126);
      }
    }
    if (!output_path.empty()) {
      const int descriptor =
          ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (descriptor >= 0) {
        ::dup2(descriptor, STDOUT_FILENO);
        ::dup2(descriptor, STDERR_FILENO);
        ::close(descriptor);
      }
    }
    ::execvp(executable.c_str(), raw.data());
    ::_exit(127);
  }
  ChildProcess child;
  child.pid_ = static_cast<std::uint32_t>(pid);
  return child;
#endif
}

bool ChildProcess::running() const noexcept { return valid() && !exited(); }

bool ChildProcess::exited() const {
  if (!valid()) {
    return true;
  }
  if (waited_) {
    return true;
  }
#ifdef _WIN32
  const DWORD result =
      ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 0);
  return result == WAIT_OBJECT_0;
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == static_cast<pid_t>(pid_);
#endif
}

Result<int> ChildProcess::wait() {
  if (!valid()) {
    return Error(ErrorCode::InvalidArgument, "process was never started");
  }
  if (waited_) {
    return exit_code_;
  }
#ifdef _WIN32
  const DWORD result = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  if (result != WAIT_OBJECT_0) {
    return Error(ErrorCode::Internal, "waiting for the process failed",
                 "windows error " + std::to_string(::GetLastError()));
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code) == 0) {
    return Error(ErrorCode::Internal, "could not read the process exit code",
                 "windows error " + std::to_string(::GetLastError()));
  }
  exit_code_ = static_cast<int>(code);
  waited_ = true;
  return exit_code_;
#else
  int status = 0;
  for (;;) {
    const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    if (result == static_cast<pid_t>(pid_)) {
      break;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return Error(ErrorCode::Internal, "waitpid failed");
  }
  if (WIFEXITED(status)) {
    exit_code_ = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code_ = 128 + WTERMSIG(status);
  } else {
    exit_code_ = -1;
  }
  waited_ = true;
  return exit_code_;
#endif
}

Status ChildProcess::kill() {
  if (!valid()) {
    return Status::success();
  }
  if (exited()) {
    return Status::success();
  }
#ifdef _WIN32
  if (::TerminateProcess(reinterpret_cast<HANDLE>(handle_), 1) == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_ACCESS_DENIED) {
      // The process is already exiting: Windows denies TerminateProcess while the last
      // thread is being torn down. Waiting for that to finish is the correct response, not
      // reporting a kill failure.
      if (::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 5000) == WAIT_OBJECT_0) {
        return Status::success();
      }
    }
    if (exited()) {
      return Status::success();
    }
    return fail(ErrorCode::Internal, "TerminateProcess failed",
                "windows error " + std::to_string(error));
  }
  return Status::success();
#else
  if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0) {
    if (errno == ESRCH) {
      return Status::success();
    }
    return fail(ErrorCode::Internal, "kill failed", std::strerror(errno));
  }
  return Status::success();
#endif
}

Result<int> ChildProcess::terminate_and_wait() {
  const Status killed = kill();
  if (!killed.ok()) {
    return killed.error();
  }
  return wait();
}

Result<std::string> current_executable_path() {
#ifdef _WIN32
  std::vector<char> buffer(512, '\0');
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return Error(ErrorCode::Internal, "GetModuleFileNameA failed",
                   "windows error " + std::to_string(::GetLastError()));
    }
    if (length < buffer.size() - 1) {
      return std::string(buffer.data(), length);
    }
    if (buffer.size() > 64 * 1024) {
      return Error(ErrorCode::BoundExceeded, "executable path is implausibly long");
    }
    buffer.resize(buffer.size() * 2, '\0');
  }
#elif defined(__linux__)
  std::vector<char> buffer(4096, '\0');
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return Error(ErrorCode::Internal, "readlink /proc/self/exe failed");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(length));
#else
  return Error(ErrorCode::UnsupportedOperation,
               "current_executable_path is not implemented on this platform");
#endif
}

Result<std::string> current_executable_directory() {
  const Result<std::string> path = current_executable_path();
  if (!path.ok()) {
    return path.error();
  }
  std::filesystem::path as_path(path.value());
  return as_path.parent_path().string();
}

std::string join_path(std::string_view base, std::string_view leaf) {
  std::filesystem::path path(base);
  path /= std::filesystem::path(leaf);
  return path.string();
}

Status ensure_directory(const std::string& path) {
  if (path.empty()) {
    return Status::success();
  }
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error && !std::filesystem::is_directory(path)) {
    return fail(ErrorCode::Internal, "could not create directory", path + ": " + error.message());
  }
  return Status::success();
}

Status remove_directory_tree(const std::string& path) {
  if (path.empty()) {
    return Status::success();
  }
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    return fail(ErrorCode::Internal, "could not remove directory tree",
                path + ": " + error.message());
  }
  return Status::success();
}

std::string unique_token() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t sequence = counter.fetch_add(1);
#ifdef _WIN32
  const std::uint64_t process = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  const std::uint64_t process = static_cast<std::uint64_t>(::getpid());
#endif
  std::ostringstream out;
  out << std::hex << process << '-' << (tick & 0xFFFFFFFFull) << '-' << sequence;
  return out.str();
}

void sleep_millis(unsigned millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

Status wait_for_file(const std::string& path, unsigned attempts, unsigned interval_millis) {
  // A startup handshake, not a deadline: it reports failure instead of blocking forever,
  // and it never bounds how long the workload under test may take.
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      const auto size = std::filesystem::file_size(path, error);
      if (!error && size > 0) {
        return Status::success();
      }
    }
    sleep_millis(interval_millis);
  }
  return fail(ErrorCode::NotFound, "timed out waiting for a startup handshake file", path);
}

Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(ErrorCode::NotFound, "could not open file", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Status write_text_file(const std::string& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return fail(ErrorCode::Internal, "could not open file for writing", path);
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  stream.flush();
  if (!stream) {
    return fail(ErrorCode::Internal, "could not write file", path);
  }
  return Status::success();
}

}  // namespace fo
