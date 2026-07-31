#include "paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace artic::ls::paths {

fs::path canonical_path(const fs::path& file) {
    std::error_code ec;
    auto path = fs::weakly_canonical(file, ec);
    if (ec) path = file.lexically_normal();

#ifdef _WIN32
    // Fold the drive letter to lower case, the spelling VS Code uses in `file:` URIs.
    // Everything else is left alone: the rest of the path is echoed back to the editor
    // in diagnostic URIs, and lowercasing it would stop those matching the open document.
    auto str = path.generic_string();
    if (str.size() >= 2 && str[1] == ':')
        str[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(str[0])));
    return fs::path(str);
#else
    return path;
#endif
}

fs::path lookup_key(const fs::path& file) {
    auto key = canonical_path(file).generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
#endif
    return key;
}

fs::path to_absolute_path(fs::path base_dir, std::string path) {
    if (path.starts_with("/")) return canonical_path(path);
    if (path.starts_with("~/")) {
        // Unset on Windows unless the user exported it; leave the `~` literal in that case.
        if (const char* home = std::getenv("HOME")) {
            base_dir = fs::path(home);
            path = path.substr(2);
        }
    }
    return canonical_path(base_dir / path);
}

std::string from_msbuild_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::optional<std::string> read_file(const fs::path& file) {
    std::ifstream is(file);
    if (!is) return std::nullopt;
    // Reading a directory throws rather than failing the stream.
    try {
        return std::make_optional(std::string(
            std::istreambuf_iterator<char>(is),
            std::istreambuf_iterator<char>()
        ));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace artic::ls::paths
