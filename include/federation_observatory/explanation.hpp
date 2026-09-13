// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "federation_observatory/export.hpp"

namespace fo {

/// Deterministic text rendering helpers. All CLI output, all explanations and all
/// digests are produced through these helpers so that a measurement taken from the CLI
/// and one taken from the C++ API cannot disagree.
[[nodiscard]] FO_API std::string indent_block(std::string_view text, std::string_view indent);

/// Fixed-width table. Column widths are computed from the content, so the same rows
/// always produce the same bytes regardless of platform.
[[nodiscard]] FO_API std::string render_table(const std::vector<std::string>& headers,
                                              const std::vector<std::vector<std::string>>& rows,
                                              std::string_view indent = "  ");

/// "key: value" line.
[[nodiscard]] FO_API std::string render_kv(std::string_view key, std::string_view value,
                                           std::string_view indent = "  ");

/// 16-character lowercase hex rendering of a 64-bit value.
[[nodiscard]] FO_API std::string hex_digest64(std::uint64_t value);

/// Stable 64-bit digest of arbitrary text, rendered as hex. The digest is a content
/// fingerprint, not a cryptographic commitment; it is used to prove determinism.
[[nodiscard]] FO_API std::string digest_text(std::string_view text);

/// Deterministic JSON-free serialization of a string list, for evidence lines.
[[nodiscard]] FO_API std::string join_strings(const std::vector<std::string>& values,
                                              std::string_view separator);

/// Truncate with an explicit marker so that a bounded rendering never silently drops
/// content.
[[nodiscard]] FO_API std::string truncate_with_marker(std::string_view text, std::size_t max_bytes);

}  // namespace fo
