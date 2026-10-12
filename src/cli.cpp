#include "hawken/cli.hpp"
#include "hawken/meteor_ini.hpp"
#include "hawken/package_integrity.hpp"
#include "hawken/sha256.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <map>
#include <ostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
constexpr auto manifest_digest = "817bee0a15c603cfc5d6d4075e8d949bcbc8a7a697e38cc21523f813a3590ef1";
constexpr auto exe_digest = "4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2";
constexpr auto sdk_digest = "f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57";

fs::path utf8_path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

fs::path executable_directory() {
#ifdef _WIN32
    std::wstring buffer(512, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) throw std::runtime_error("Cannot locate bundled profile");
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) throw std::runtime_error("Cannot locate bundled profile");
        buffer.resize(buffer.size() * 2);
    }
#else
    std::string buffer(512, '\0');
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) throw std::runtime_error("Cannot locate bundled profile");
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return utf8_path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) throw std::runtime_error("Cannot locate bundled profile");
        buffer.resize(buffer.size() * 2);
    }
#endif
}

bool digest_valid(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

json integrity_json(const hawken::PackageIntegrityReport& report) {
    json issues = json::array();
    for (const auto& issue : report.issues) issues.push_back({
        {"code", issue.code}, {"relativePath", issue.relative_path}, {"detail", issue.detail}});
    return {{"passed", report.passed}, {"manifestSha256", report.manifest_sha256},
        {"verifiedCount", report.verified_count}, {"issues", issues},
        {"scope", "LOCAL_FILE_SNAPSHOT"}, {"remoteAttestation", false}};
}

json binary_identity(const fs::path& path, const char* expected) {
    try {
        const auto digest = hawken::sha256_file(path);
        return {{"sha256", digest}, {"matchesResearchBuild", digest == expected}};
    } catch (const std::exception&) {
        return {{"errorCode", "read_failed"}, {"matchesResearchBuild", false}};
    }
}

json preflight(const fs::path& root, const hawken::PackageIntegrityReport& integrity) {
    const auto binaries = root / "Binaries" / "Win64";
    json ini;
    try {
        const auto setting = hawken::read_meteor_ini(binaries / "StormConsumerSettings.ini");
        const auto path_start = setting.uri.find('/', setting.uri.find("://") + 3);
        ini = {{"uriOrigin", setting.uri.substr(0, path_start)},
            {"basePathRedacted", path_start != std::string::npos}, {"lineNumber", setting.line_number},
            {"byteOffset", setting.byte_offset}, {"encoding", setting.encoding_name}};
    } catch (const hawken::MeteorIniError& exception) {
        ini = {{"errorCode", exception.code}};
    }
    const auto exe = binary_identity(binaries / "HawkenGame-Win64-Shipping.exe", exe_digest);
    const auto sdk = binary_identity(binaries / "Meteor.Sdk.Cpp.Win32.Client64.dll", sdk_digest);
    const bool researched = exe.at("matchesResearchBuild").get<bool>() &&
        sdk.at("matchesResearchBuild").get<bool>();
    return {{"schema", "hawken.preflight.v1"}, {"readyForPlayers", false},
        {"packageIntegrity", integrity_json(integrity)}, {"meteorIni", ini},
        {"clientBuild", {{"executable", exe}, {"sdk", sdk}}},
        {"effectiveEndpoint", {{"status", researched ? "BLOCKED_STATIC_OVERRIDE" : "UNSUPPORTED_BUILD"},
            {"runtimeVerified", false}, {"evidence", {{"executableSha256", exe_digest},
                {"callerRva", "0xbc3510"}, {"sdkSha256", sdk_digest}, {"setterRva", "0x74810"}}}}},
        {"steamAuthentication", {{"status", "NOT_VERIFIED"}}},
        {"gameServer", {{"status", "NOT_VERIFIED"}}}};
}

int usage(std::ostream& error) {
    error << "Usage: hawken-tools verify-packages|preflight --game <directory> "
             "[--manifest <file> --manifest-sha256 <trusted SHA-256>]\n";
    return 64;
}
}

namespace hawken {
int run_cli(const std::vector<std::string>& args, std::ostream& output, std::ostream& error) {
    if (args.empty() || (args[0] != "verify-packages" && args[0] != "preflight")) return usage(error);
    std::map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || args[i + 1].empty() ||
            (args[i] != "--game" && args[i] != "--manifest" && args[i] != "--manifest-sha256") ||
            !options.emplace(args[i], args[i + 1]).second) return usage(error);
    }
    if (!options.contains("--game") ||
        options.contains("--manifest") != options.contains("--manifest-sha256") ||
        (options.contains("--manifest-sha256") && !digest_valid(options.at("--manifest-sha256"))))
        return usage(error);
    try {
        const auto root = utf8_path(options.at("--game"));
        const auto manifest = options.contains("--manifest") ? utf8_path(options.at("--manifest")) :
            executable_directory() / "profiles" / "original-upk-sha256-manifest.json";
        auto trusted = options.contains("--manifest-sha256") ? options.at("--manifest-sha256") :
            std::string(manifest_digest);
        std::transform(trusted.begin(), trusted.end(), trusted.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
        });
        const auto integrity = verify_packages(root, manifest, trusted);
        output << (args[0] == "preflight" ? preflight(root, integrity) : integrity_json(integrity)).dump(2) << '\n';
        return args[0] == "preflight" ? 2 : (integrity.passed ? 0 : 1);
    } catch (const std::exception&) {
        output << json{{"passed", false}, {"readyForPlayers", false}, {"errorCode", "diagnostic_failed"}}.dump() << '\n';
        error << "Diagnostic failed; no game files were changed.\n";
        return args[0] == "preflight" ? 2 : 1;
    }
}
}
