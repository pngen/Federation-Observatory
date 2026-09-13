// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <string_view>

#include "federation_observatory/export.hpp"

namespace fo {

/// Semantic version of the Federation Observatory runtime.
struct Version {
  int major = FO_VERSION_MAJOR;
  int minor = FO_VERSION_MINOR;
  int patch = FO_VERSION_PATCH;
};

/// Compile-time version string of the runtime the caller linked against.
[[nodiscard]] FO_API std::string_view version_string() noexcept;

/// Numeric version of the runtime the caller linked against.
[[nodiscard]] FO_API Version version() noexcept;

/// Machine-readable capability banner: version, protocol revision, and encoded width.
[[nodiscard]] FO_API std::string_view build_banner() noexcept;

/// Persistence format revision understood by this build.
[[nodiscard]] FO_API std::uint32_t persistence_format_version() noexcept;

/// Wire protocol revision understood by this build.
[[nodiscard]] FO_API std::uint32_t protocol_version() noexcept;

}  // namespace fo
