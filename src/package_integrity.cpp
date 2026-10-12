#include "hawken/package_integrity.hpp"
#include "hawken/sha256.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <map>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace hawken {
namespace {
namespace fs = std::filesystem;

// Local verifier policies, not limits attributed to the original game or SDK.
constexpr std::size_t max_manifest_bytes = 4U * 1024U * 1024U;
constexpr std::size_t max_manifest_entries = 4096;
constexpr int max_json_containers = 16;
constexpr std::size_t max_scan_depth = 64;
constexpr std::size_t max_scan_entries = 100000;
struct JsonResourceLimit {};

std::string ascii_lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

bool is_sha256(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

void issue(PackageIntegrityReport& report, const std::string& code,
           const std::string& relative, const std::string& detail) {
    report.issues.push_back({code, relative, detail});
}

enum class PathState { regular, missing, link, unreadable };

PathState inspect_path(const fs::path& path, fs::file_status& status) {
    std::error_code error;
    status = fs::symlink_status(path, error);
    if (fs::is_symlink(status)) return PathState::link;
    if (error == std::errc::no_such_file_or_directory || !fs::exists(status)) {
        return error && error != std::errc::no_such_file_or_directory
            ? PathState::unreadable : PathState::missing;
    }
    if (error) return PathState::unreadable;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return PathState::unreadable;
    // This also rejects junctions and reparse types not represented as C++ symlinks.
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return PathState::link;
#endif
    return PathState::regular;
}

bool inspect_ancestors(const fs::path& path, PackageIntegrityReport& report) {
    if (path.empty()) {
        issue(report, "READ_ERROR", "", "An input path is empty.");
        return false;
    }
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error) {
        issue(report, "READ_ERROR", "", "Cannot resolve an input path.");
        return false;
    }
    fs::path current = absolute.root_path();
    fs::file_status status;
    auto inspect = [&]() {
        const auto state = inspect_path(current, status);
        if (state == PathState::regular) return true;
        issue(report, state == PathState::link ? "LINK_REJECTED" : "READ_ERROR", "",
              state == PathState::link ? "An input path traverses a link or reparse point."
                                      : "Cannot inspect an input path.");
        return false;
    };
    if (!inspect()) return false;
    for (const auto& component : absolute.relative_path()) {
        current /= component;
        if (!inspect()) return false;
    }
    return true;
}

bool package_extension(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const auto extension = ascii_lower(path.substr(dot));
    return extension == ".upk" || extension == ".u" || extension == ".udk";
}

bool valid_relative_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || !package_extension(path)) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == std::string::npos ? end : end - start);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == ' ' || component.back() == '.') return false;
        for (char character : component) {
            const auto byte = static_cast<unsigned char>(character);
            // M1's trusted Windows baseline uses ASCII names. Fail closed instead of
            // pretending that ASCII folding handles Unicode Windows aliases on Linux.
            if (byte < 32 || byte >= 127 || character == ':' || character == '<' ||
                character == '>' || character == '"' || character == '|' || character == '?' ||
                character == '*') return false;
        }
        // Reject Windows device aliases, including names with an extension.
        const auto device = ascii_lower(component.substr(0, component.find('.')));
        if (device == "con" || device == "prn" || device == "aux" || device == "nul" ||
            (device.size() == 4 && (device.starts_with("com") || device.starts_with("lpt")) &&
             device[3] >= '1' && device[3] <= '9')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == path.size()) return false;
    }
    return true;
}

bool unsigned_integer(const nlohmann::json& value, std::uintmax_t& result) {
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_number_integer()) return false;
    const auto number = value.get<std::int64_t>();
    if (number < 0) return false;
    result = static_cast<std::uintmax_t>(number);
    return true;
}

struct ExpectedPackage {
    std::string path;
    std::string hash;
    std::uintmax_t size;
};

bool parse_baseline(const std::string& bytes, PackageIntegrityReport& report,
                    std::map<std::string, ExpectedPackage>& expected) {
    nlohmann::json manifest;
    try {
        const auto depth_guard = [](int depth, nlohmann::json::parse_event_t event, nlohmann::json&) {
            // Callback depth is zero-based; the root is container level one.
            if (depth >= max_json_containers &&
                (event == nlohmann::json::parse_event_t::object_start ||
                 event == nlohmann::json::parse_event_t::array_start)) {
                throw JsonResourceLimit{};
            }
            return true;
        };
        manifest = nlohmann::json::parse(bytes, depth_guard, false);
    } catch (const JsonResourceLimit&) {
        issue(report, "MANIFEST_RESOURCE_LIMIT", "", "JSON exceeds 16 container levels including the root.");
        return false;
    }
    if (manifest.is_object() && manifest.contains("entries") && manifest["entries"].is_array() &&
        manifest["entries"].size() > max_manifest_entries) {
        issue(report, "MANIFEST_RESOURCE_LIMIT", "", "Baseline exceeds 4096 package entries.");
        return false;
    }
    std::uintmax_t count = 0;
    if (!manifest.is_object() || manifest.value("schema", nlohmann::json()) !=
            "hawken.original-package-sha256-baseline.v1" ||
        manifest.value("hash_algorithm", nlohmann::json()) != "SHA-256" ||
        !manifest.contains("entries") || !manifest["entries"].is_array() ||
        manifest["entries"].empty() || !manifest.contains("package_count") ||
        !unsigned_integer(manifest["package_count"], count) || count != manifest["entries"].size()) {
        issue(report, "MANIFEST_INVALID", "", "Invalid baseline schema, algorithm, entries or count.");
        return false;
    }

    for (const auto& item : manifest["entries"]) {
        std::string relative;
        if (item.is_object() && item.contains("relative_path") && item["relative_path"].is_string()) {
            relative = item["relative_path"].get<std::string>();
            std::replace(relative.begin(), relative.end(), '\\', '/');
        } else {
            issue(report, "MANIFEST_INVALID", "", "A baseline entry has no string relative path.");
            continue;
        }
        if (!valid_relative_path(relative)) {
            issue(report, "MANIFEST_PATH_INVALID", relative, "Unsafe or unsupported package path.");
            continue;
        }
        const auto key = ascii_lower(relative);
        if (expected.contains(key)) {
            issue(report, "MANIFEST_PATH_DUPLICATE", relative, "Duplicate or case-colliding package path.");
            continue;
        }
        std::uintmax_t size = 0;
        if (!item.contains("sha256") || !item["sha256"].is_string() ||
            !is_sha256(item["sha256"].get<std::string>()) || !item.contains("size_bytes") ||
            !unsigned_integer(item["size_bytes"], size)) {
            issue(report, "MANIFEST_INVALID", relative, "Invalid expected SHA-256 or byte size.");
            continue;
        }
        expected.emplace(key, ExpectedPackage{relative, ascii_lower(item["sha256"].get<std::string>()), size});
    }
    return report.issues.empty();
}

struct FoundPackage {
    fs::path absolute;
    std::string relative;
};

bool scan_packages(const fs::path& root, PackageIntegrityReport& report,
                   std::map<std::string, FoundPackage>& found) {
    struct PendingDirectory { fs::path path; std::size_t depth; };
    std::vector<PendingDirectory> pending{{root, 0}};
    std::size_t scanned_entries = 0;
    while (!pending.empty()) {
        const auto directory = pending.back();
        pending.pop_back();
        std::error_code error;
        fs::directory_iterator iterator(directory.path, fs::directory_options::none, error);
        if (error) {
            issue(report, "READ_ERROR", path_text(directory.path.lexically_relative(root)),
                  "Cannot enumerate a package directory.");
            continue;
        }
        const fs::directory_iterator end;
        while (iterator != end) {
            const auto path = iterator->path();
            const auto relative = path_text(path.lexically_relative(root));
            // Count every entry, including non-packages and links; the root itself is depth zero.
            if (scanned_entries == max_scan_entries) {
                issue(report, "SCAN_RESOURCE_LIMIT", relative, "Tree exceeds 100000 file and directory entries.");
                return false;
            }
            ++scanned_entries;
            const auto depth = directory.depth + 1;
            if (depth > max_scan_depth) {
                issue(report, "SCAN_RESOURCE_LIMIT", relative, "Tree entry exceeds depth 64 from root depth zero.");
                return false;
            }
            fs::file_status status;
            const auto state = inspect_path(path, status);
            if (state == PathState::link) {
                issue(report, "LINK_REJECTED", relative, "Link or reparse point rejected without traversal.");
            } else if (state != PathState::regular) {
                issue(report, "READ_ERROR", relative, "Cannot inspect a directory entry.");
            } else if (fs::is_directory(status)) {
                pending.push_back({path, depth});
            } else if (package_extension(relative)) {
                if (!fs::is_regular_file(status)) {
                    issue(report, "READ_ERROR", relative, "Package is not a regular file.");
                } else if (!found.emplace(ascii_lower(relative), FoundPackage{path, relative}).second) {
                    issue(report, "PACKAGE_PATH_DUPLICATE", relative, "Case-colliding package files on disk.");
                }
            }
            iterator.increment(error);
            if (error) {
                issue(report, "READ_ERROR", path_text(directory.path.lexically_relative(root)),
                      "Cannot finish enumerating a package directory.");
                break;
            }
        }
    }
    return true;
}
}

static PackageIntegrityReport verify_packages_impl(const std::filesystem::path& game_root,
                                                   const std::filesystem::path& manifest_path,
                                                   const std::string& trusted_manifest_sha256) {
    PackageIntegrityReport report;
    if (!inspect_ancestors(manifest_path, report)) return report;

    std::error_code error;
    if (!fs::is_regular_file(manifest_path, error) || error) {
        issue(report, "READ_ERROR", "", "Baseline manifest is not a readable regular file.");
        return report;
    }
    const auto manifest_size = fs::file_size(manifest_path, error);
    if (error) {
        issue(report, "READ_ERROR", "", "Cannot read the baseline manifest byte size.");
        return report;
    }
    if (manifest_size > max_manifest_bytes) {
        issue(report, "MANIFEST_RESOURCE_LIMIT", "", "Baseline manifest exceeds 4 MiB.");
        return report;
    }

    std::ifstream stream(manifest_path, std::ios::binary);
    if (!stream) {
        issue(report, "READ_ERROR", "", "Cannot open the baseline manifest.");
        return report;
    }
    std::string bytes;
    std::array<char, 8192> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read_count = static_cast<std::size_t>(stream.gcount());
        // Bound the bytes actually read too, in case the manifest grows after file_size.
        if (read_count > max_manifest_bytes - bytes.size()) {
            issue(report, "MANIFEST_RESOURCE_LIMIT", "", "Read manifest bytes exceed 4 MiB.");
            return report;
        }
        bytes.append(buffer.data(), read_count);
    }
    if (stream.bad() || !stream.eof() || bytes.size() != manifest_size) {
        issue(report, "READ_ERROR", "", "Cannot read the complete baseline manifest.");
        return report;
    }
    try {
        // Hash the same captured bytes that will be parsed; do not reopen the manifest.
        report.manifest_sha256 = sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    } catch (const std::exception&) {
        issue(report, "READ_ERROR", "", "Cannot hash the baseline manifest.");
        return report;
    }
    if (!is_sha256(trusted_manifest_sha256)) {
        issue(report, "MANIFEST_INVALID", "", "The explicitly trusted digest is not SHA-256 hexadecimal.");
        return report;
    }
    if (report.manifest_sha256 != ascii_lower(trusted_manifest_sha256)) {
        issue(report, "MANIFEST_HASH_MISMATCH", "", "Manifest bytes differ from the explicitly trusted digest.");
        return report;
    }

    std::map<std::string, ExpectedPackage> expected;
    if (!parse_baseline(bytes, report, expected)) return report;
    if (!inspect_ancestors(game_root, report)) return report;
    if (!fs::is_directory(game_root, error) || error) {
        issue(report, "READ_ERROR", "", "Game root is not a readable directory.");
        return report;
    }

    std::map<std::string, FoundPackage> found;
    // Resource aborts stop before hashing any package; existing non-cap issues retain their behavior.
    if (!scan_packages(game_root, report, found)) return report;
    for (const auto& [key, package] : found) {
        if (!expected.contains(key)) {
            issue(report, "PACKAGE_EXTRA", package.relative, "Package is not in the trusted baseline.");
        }
    }
    for (const auto& [key, package] : expected) {
        const auto actual = found.find(key);
        if (actual == found.end()) {
            issue(report, "PACKAGE_MISSING", package.path, "Expected regular package was not found.");
            continue;
        }
        const auto size = fs::file_size(actual->second.absolute, error);
        if (error) {
            issue(report, "READ_ERROR", package.path, "Cannot read the package byte size.");
            continue;
        }
        if (size != package.size) {
            issue(report, "PACKAGE_SIZE_MISMATCH", package.path, "Package byte size differs from the baseline.");
            continue;
        }
        try {
            if (sha256_file(actual->second.absolute) != package.hash) {
                issue(report, "PACKAGE_HASH_MISMATCH", package.path, "Package SHA-256 differs from the baseline.");
                continue;
            }
            ++report.verified_count;
        } catch (const std::exception&) {
            issue(report, "READ_ERROR", package.path, "Cannot completely read and hash the package.");
        }
    }
    report.passed = report.issues.empty() && report.verified_count == expected.size();
    return report;
}

PackageIntegrityReport verify_packages(const std::filesystem::path& game_root,
                                       const std::filesystem::path& manifest_path,
                                       const std::string& trusted_manifest_sha256) {
    try {
        return verify_packages_impl(game_root, manifest_path, trusted_manifest_sha256);
    } catch (const fs::filesystem_error&) {
        PackageIntegrityReport report;
        issue(report, "READ_ERROR", "", "Cannot inspect or represent a filesystem path.");
        return report;
    } catch (const nlohmann::json::exception&) {
        PackageIntegrityReport report;
        issue(report, "MANIFEST_INVALID", "", "Cannot parse the baseline structure.");
        return report;
    }
}
}
