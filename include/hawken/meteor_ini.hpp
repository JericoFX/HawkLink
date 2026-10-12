#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace hawken {
struct MeteorIniSetting {
    std::string uri;
    std::size_t line_number;
    std::uint64_t byte_offset;
    std::string encoding_name;
};

class MeteorIniError : public std::runtime_error {
public:
    explicit MeteorIniError(std::string error_code);
    std::string code;
};

MeteorIniSetting read_meteor_ini(const std::filesystem::path& path);
}
