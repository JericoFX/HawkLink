#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace hawken::test {
class TempDirectory {
public:
    TempDirectory() {
        for (int attempt = 0; attempt < 16; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                ("Hawken-Test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(std::random_device{}()));
            if (std::filesystem::create_directory(path_)) return;
        }
        throw std::runtime_error("Cannot create isolated test directory");
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path write(const std::filesystem::path& relative, const std::string& bytes) const {
        const auto target = path_ / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("Cannot write fixture");
        return target;
    }
private:
    std::filesystem::path path_;
};
inline std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
}
