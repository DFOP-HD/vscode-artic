#include "json_source.h"

#include "paths.h"
#include <algorithm>

namespace artic::ls {

std::string json_pointer_token(std::string_view name) {
    std::string out;
    out.reserve(name.size() + 1);
    for (char c : name) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out.push_back(c);
    }
    return out;
}

namespace {

/// A JSON reader that keeps offsets and throws the values away -- the opposite of what
/// nlohmann does, and the reason both have to run.
///
/// It never reports an error: nlohmann is the authority on whether the document is valid,
/// and this pass has to survive being handed the invalid one so a syntax error can still be
/// placed. Every loop is written to make progress on any input.
class Scanner {
public:
    Scanner(std::string_view text, std::unordered_map<std::string, std::pair<size_t, size_t>>& spans)
        : text_(text), spans_(spans) {}

    void run() { value("", 0, npos); }

private:
    static constexpr size_t npos = std::string_view::npos;
    // Deep enough for any configuration, shallow enough not to overflow the stack.
    static constexpr size_t max_depth = 64;

    bool at(char c) const { return pos_ < text_.size() && text_[pos_] == c; }

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    /// Consumes a string literal and returns its content. `\u` is left encoded: a pointer
    /// token only has to match what nlohmann produced, non-ASCII keys do not occur in a
    /// configuration, and a mismatch costs nothing but the fallback.
    std::string string_literal() {
        std::string out;
        if (!at('"')) return out;
        ++pos_;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            if (text_[pos_] != '\\' || pos_ + 1 >= text_.size()) {
                out.push_back(text_[pos_++]);
                continue;
            }
            ++pos_;
            char c = text_[pos_++];
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u':
                    out += "\\u";
                    for (int i = 0; i < 4 && pos_ < text_.size(); ++i) out.push_back(text_[pos_++]);
                    break;
                default: out.push_back(c);
            }
        }
        if (pos_ < text_.size()) ++pos_; // closing quote
        return out;
    }

    void value(const std::string& pointer, size_t depth, size_t span_start) {
        skip_ws();
        if (span_start == npos) span_start = pos_;
        if (depth >= max_depth) { skip_atom(); return; }

        if (at('{')) object(pointer, depth);
        else if (at('[')) array(pointer, depth);
        else if (at('"')) string_literal();
        else skip_atom();

        spans_.insert_or_assign(pointer, std::pair{ span_start, pos_ });
    }

    void object(const std::string& pointer, size_t depth) {
        ++pos_;
        while (pos_ < text_.size()) {
            skip_ws();
            if (at('}')) { ++pos_; return; }
            if (!at('"')) { ++pos_; continue; } // garbage before a member name
            auto key_start = pos_;
            auto key = string_literal();
            skip_ws();
            if (!at(':')) continue;
            ++pos_;
            value(pointer + "/" + json_pointer_token(key), depth + 1, key_start);
            skip_ws();
            if (at(',')) { ++pos_; continue; }
            if (at('}')) { ++pos_; return; }
            return; // malformed; nlohmann reports it
        }
    }

    void array(const std::string& pointer, size_t depth) {
        ++pos_;
        for (size_t index = 0; pos_ < text_.size(); ++index) {
            skip_ws();
            if (at(']')) { ++pos_; return; }
            value(pointer + "/" + std::to_string(index), depth + 1, npos);
            skip_ws();
            if (at(',')) { ++pos_; continue; }
            if (at(']')) { ++pos_; return; }
            return;
        }
    }

    /// A number, `true`, `false`, `null`, or whatever else is there.
    void skip_atom() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
            ++pos_;
        }
    }

    std::string_view text_;
    std::unordered_map<std::string, std::pair<size_t, size_t>>& spans_;
    size_t pos_ = 0;
};

} // anonymous namespace

JsonSource JsonSource::scan(std::string_view text) {
    JsonSource source;
    source.size_ = text.size();
    source.line_starts_.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') source.line_starts_.push_back(i + 1);

    Scanner(text, source.spans_).run();
    return source;
}

std::optional<JsonSource> JsonSource::read(const fs::path& file) {
    auto text = paths::read_file(file);
    if (!text) return std::nullopt;
    return scan(*text);
}

lsp::Position JsonSource::position_of(size_t offset) const {
    offset = std::min(offset, size_);
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

std::optional<lsp::Range> JsonSource::range(const std::string& pointer) const {
    auto it = spans_.find(pointer);
    if (it == spans_.end()) return std::nullopt;
    return lsp::Range{ position_of(it->second.first), position_of(it->second.second) };
}

} // namespace artic::ls
