#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace hawken {
struct IntegrityIssue {
    std::string code;
    std::string relative_path;
    std::string detail;
};

struct PackageIntegrityReport {
    bool passed = false;
    std::string manifest_sha256;
    // Packages that matched both size and SHA-256, even if other issues fail the report.
    std::size_t verified_count = 0;
    std::vector<IntegrityIssue> issues;
};

// Read-only local snapshot. The caller supplies trust; baseline paths are ASCII in M1.
PackageIntegrityReport verify_packages(const std::filesystem::path& game_root,
                                       const std::filesystem::path& manifest_path,
                                       const std::string& trusted_manifest_sha256);
}
