#include "json_source.h"

#include "paths.h"
#include <algorithm>
#include <cctype>

#if !JSON_DIAGNOSTIC_POSITIONS
#error "artic-lsp needs nlohmann's diagnostic positions; set JSON_Diagnostic_Positions=ON"
#endif

namespace artic::ls {

JsonSource::JsonSource(std::string text)
    : text_(std::move(text))
{
    line_starts_.push_back(0);
    for (size_t i = 0; i < text_.size(); ++i)
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
}

std::optional<JsonSource> JsonSource::read(const fs::path& file) {
    auto text = paths::read_file(file);
    if (!text) return std::nullopt;
    return JsonSource(std::move(*text));
}

lsp::Position JsonSource::position_of(size_t offset) const {
    offset = std::min(offset, text_.size());
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    auto line = static_cast<size_t>(it - line_starts_.begin()) - 1;
    return lsp::Position{
        static_cast<lsp::uint>(line),
        static_cast<lsp::uint>(offset - line_starts_[line]),
    };
}

lsp::Position JsonSource::position_of_byte(size_t byte) const {
    return position_of(byte == 0 ? 0 : byte - 1);
}

std::optional<lsp::Range> JsonSource::value_range(const nlohmann::json& value) const {
    auto start = value.start_pos();
    auto end = value.end_pos();
    if (start == std::string::npos || end == std::string::npos || start > end) return std::nullopt;
    return lsp::Range{ position_of(start), position_of(end) };
}

std::optional<lsp::Range> JsonSource::member_range(const nlohmann::json& value) const {
    auto range = value_range(value);
    if (!range) return range;

    auto skip_space_back = [&](size_t i) {
        while (i > 0 && std::isspace(static_cast<unsigned char>(text_[i - 1]))) --i;
        return i;
    };

    auto i = skip_space_back(value.start_pos());
    if (i == 0 || text_[i - 1] != ':') return range;
    i = skip_space_back(i - 1);
    if (i == 0 || text_[i - 1] != '"') return range;

    // Walk back to the quote that opened the name; a `"` is part of it when escaped.
    size_t quote = i - 1;
    while (quote > 0) {
        --quote;
        if (text_[quote] != '"') continue;
        size_t slashes = 0;
        while (quote > slashes && text_[quote - 1 - slashes] == '\\') ++slashes;
        if (slashes % 2 == 0) return lsp::Range{ position_of(quote), range->end };
    }
    return range;
}

} // namespace artic::ls
