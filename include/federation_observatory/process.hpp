// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/export.hpp"

namespace fo {

/// A real operating-system child process. Used by the CLI, by the multiprocess proofs
/// and by the process-death tests. There is no thread-based imitation anywhere in the
/// runtime: publisher-death proof requires an actual process death.
class FO_API ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Spawn \p argv[0] with the remaining arguments. \p workdir may be empty to
  /// inherit the parent's working directory. When \p output_path is non-empty the child's
  /// standard output and standard error are redirected to that file, which is what makes a
  /// failed child diagnosable from a test.
  [[nodiscard]] static Result<ChildProcess> spawn(const std::vector<std::string>& argv,
                                                  const std::string& workdir = std::string(),
                                                  const std::string& output_path = std::string());

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint32_t pid() const noexcept { return pid_; }
  [[nodiscard]] bool valid() const noexcept { return pid_ != 0; }

  /// Block until the child exits and return its exit code. Returns an error when the
  /// child was never started. Safe to call once; later calls return the cached code.
  [[nodiscard]] Result<int> wait();

  /// Forcefully terminate the child. Idempotent. Does not wait.
  [[nodiscard]] Status kill();

  /// Terminate and then reap, returning the exit code.
  [[nodiscard]] Result<int> terminate_and_wait();

  /// Non-blocking check for whether the process has exited.
  [[nodiscard]] bool exited() const;

 private:
  void release() noexcept;

  std::uint32_t pid_ = 0;
  std::intptr_t handle_ = -1;
  bool waited_ = false;
  int exit_code_ = -1;
};

/// Fully qualified path of the running executable.
[[nodiscard]] FO_API Result<std::string> current_executable_path();

/// Directory containing the running executable.
[[nodiscard]] FO_API Result<std::string> current_executable_directory();

/// Join two path fragments using the platform separator.
[[nodiscard]] FO_API std::string join_path(std::string_view base, std::string_view leaf);

/// Create a directory and every missing parent. Existing directories are accepted.
[[nodiscard]] FO_API Status ensure_directory(const std::string& path);

/// Remove a directory tree, ignoring absence.
[[nodiscard]] FO_API Status remove_directory_tree(const std::string& path);

/// A process-unique token, used for temporary file names.
[[nodiscard]] FO_API std::string unique_token();

/// Sleep for \p millis milliseconds. Used for startup handshakes, never for test
/// deadlines.
FO_API void sleep_millis(unsigned millis);

/// Wait until \p path exists and is non-empty, or until \p attempts polls have been
/// made. Returns NotFound when the condition never held. This is a startup handshake
/// helper: it reports failure rather than blocking forever.
[[nodiscard]] FO_API Status wait_for_file(const std::string& path, unsigned attempts,
                                          unsigned interval_millis);

/// Read a whole small text file. Used to surface a child process's output when it fails.

[[nodiscard]] FO_API Result<std::string> read_text_file(const std::string& path);

/// Write a whole small text file.
[[nodiscard]] FO_API Status write_text_file(const std::string& path, std::string_view content);

}  // namespace fo
