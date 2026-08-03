#ifndef ARTIC_LS_JSON_SOURCE_H
#define ARTIC_LS_JSON_SOURCE_H

#include "lsp/types.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace artic::ls {

namespace fs = std::filesystem;

/// The text a JSON document was parsed from, so a value can be reported where it was written.
///
/// nlohmann records `start_pos()`/`end_pos()` on every parsed value when
/// JSON_DIAGNOSTIC_POSITIONS is on -- enabled for the whole build in
/// cmake/Dependencies.cmake. Those are byte offsets into the input, which is why the input
/// has to be kept: without it a config diagnostic can only be placed by searching the
/// document for the value, and that reports every textually identical string.
class JsonSource {
public:
    JsonSource() = default;
    explicit JsonSource(std::string text);
    static std::optional<JsonSource> read(const fs::path& file);

    const std::string& text() const { return text_; }

    lsp::Position position_of(size_t offset) const;
    /// Position of a byte as `nlohmann::json::parse_error::byte` reports it: 1-based, and
    /// one past the offending character.
    lsp::Position position_of_byte(size_t byte) const;

    /// Where a value was written, or nothing if it did not come from a parse.
    std::optional<lsp::Range> value_range(const nlohmann::json& value) const;

    /// The same, extended back over the `"name":` introducing it, so a message about a
    /// member reports the member and not merely what it was set to. An array element has
    /// no such prefix and is returned unchanged.
    std::optional<lsp::Range> member_range(const nlohmann::json& value) const;

private:
    std::string text_;
    std::vector<size_t> line_starts_;
};

} // namespace artic::ls

#endif // ARTIC_LS_JSON_SOURCE_H
