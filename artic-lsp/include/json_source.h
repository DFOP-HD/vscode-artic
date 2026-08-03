#ifndef ARTIC_LS_JSON_SOURCE_H
#define ARTIC_LS_JSON_SOURCE_H

#include "lsp/types.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace artic::ls {

namespace fs = std::filesystem;

/// Where each value of a JSON document was written.
///
/// nlohmann/json keeps no source information: a parsed value cannot say which line it came
/// from, and there is no option that makes it. Anything that has to point back at the
/// document therefore has to find its own way there, and searching the text for the value
/// is the wrong one -- a `files` pattern used by two projects, or a project named after a
/// dependency, matches in more than one place and every match gets reported.
///
/// So the document is scanned a second time and indexed by RFC 6901 pointer
/// (`/projects/0/files/2`). The scan is deliberately tolerant: it is only ever run on text
/// nlohmann has already accepted or is about to reject, and a pointer that fails to resolve
/// simply falls back to the old search.
class JsonSource {
public:
    JsonSource() = default;
    static JsonSource scan(std::string_view text);
    static std::optional<JsonSource> read(const fs::path& file);

    /// The range a value was written in, spanning its member name where it has one, so that
    /// `"folder": "src"` is reported rather than a bare `"src"`.
    std::optional<lsp::Range> range(const std::string& pointer) const;

    /// Position of a byte offset, as `nlohmann::json::parse_error::byte` reports it (1-based,
    /// and one past the offending character).
    lsp::Position position_of_byte(size_t byte) const;

    bool empty() const { return spans_.empty(); }

private:
    lsp::Position position_of(size_t offset) const;

    std::unordered_map<std::string, std::pair<size_t, size_t>> spans_;
    std::vector<size_t> line_starts_;
    size_t size_ = 0;
};

/// Escapes one path segment for use in an RFC 6901 pointer.
std::string json_pointer_token(std::string_view name);

} // namespace artic::ls

#endif // ARTIC_LS_JSON_SOURCE_H
