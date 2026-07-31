#ifndef ARTIC_LS_PATHS_H
#define ARTIC_LS_PATHS_H

#include <filesystem>
#include <optional>
#include <string>

namespace artic::ls::paths {

namespace fs = std::filesystem;

// The canonical spelling of a path. This is a *file's identity*: it is what gets handed to
// the lexer and the locator, so every `Loc::file` string and every diagnostic URI derives
// from it, and any lookup keyed by path compares against it.
//
// Producers disagree about the spelling of the drive letter on Windows: VS Code always
// sends `d:/...` in `file:` URIs, while CMake-generated `.vcxproj` files contain `D:\...`.
// `weakly_canonical` normalises neither case nor drive letter, so without this fold the
// same file gets two identities depending on which producer registered it first, and
// semantic tokens, inlay hints and go-to-definition all silently return nothing.
fs::path canonical_path(const fs::path& file);

// Key used to look files up. On Windows paths are case-insensitive, so the key is
// lowercased to avoid tracking the same file twice. It must never be used as the file's
// identity: diagnostics are published under File::path, and a lowercased URI does not
// match the one the editor opened.
fs::path lookup_key(const fs::path& file);

// Resolves `path` against `base_dir`, expanding a leading `~/`.
fs::path to_absolute_path(fs::path base_dir, std::string_view path);

// .sln and .vcxproj are Windows-only formats and always spell paths with backslashes,
// which are ordinary filename characters everywhere else.
std::string from_msbuild_path(std::string path);

// Nothing when the file cannot be read, including when it is a directory.
std::optional<std::string> read_file(const fs::path& file);

} // namespace artic::ls::paths

#endif // ARTIC_LS_PATHS_H
