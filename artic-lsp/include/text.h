#ifndef ARTIC_LS_TEXT_H
#define ARTIC_LS_TEXT_H

#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>

namespace artic::ls::text {

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline std::string_view trim_left(std::string_view s) {
    auto pos = s.find_first_not_of(" \t");
    return pos == std::string_view::npos ? std::string_view{} : s.substr(pos);
}

// Ninja commands are wrapped in `cmd.exe /C "..."`, so tokens carry stray quotes.
inline std::string_view strip_quotes(std::string_view s) {
    while (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.remove_suffix(1);
    return s;
}

inline std::vector<std::string> split_whitespace(std::string_view s) {
    std::vector<std::string> tokens;
    std::istringstream ss{std::string(s)};
    std::string token;
    while (ss >> token) tokens.push_back(std::move(token));
    return tokens;
}

// Splits a command line, honouring quotes so a path containing spaces stays one token.
// Quoting may start mid-token (`-o"my file.o"`), exactly as a shell allows.
inline std::vector<std::string> split_command_line(std::string_view s) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    char quote = '\0';
    for (char c : s) {
        if (quote) {
            if (c == quote) quote = '\0';
            else current.push_back(c);
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_token = true;
        } else if (c == ' ' || c == '\t') {
            if (!in_token) continue;
            tokens.push_back(std::move(current));
            current.clear();
            in_token = false;
        } else {
            current.push_back(c);
            in_token = true;
        }
    }
    if (in_token) tokens.push_back(std::move(current));
    return tokens;
}

inline std::string quote(std::string_view s) {
    return "\"" + std::string(s) + "\"";
}

} // namespace artic::ls::text

#endif // ARTIC_LS_TEXT_H
