#include "hawken/sha256.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace {
#ifdef _WIN32
void require_crypto(NTSTATUS status) {
    if (status < 0) throw std::runtime_error("SHA-256 provider failed");
}

struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    Algorithm() {
        require_crypto(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
};
struct Hash {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~Hash() { if (handle) BCryptDestroyHash(handle); }
};
#endif

class Digest {
public:
    Digest() {
#ifdef _WIN32
        DWORD object_size = 0, digest_size = 0, received = 0;
        require_crypto(BCryptGetProperty(algorithm_.handle, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0));
        if (received != sizeof(object_size)) throw std::runtime_error("SHA-256 provider failed");
        require_crypto(BCryptGetProperty(algorithm_.handle, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size), &received, 0));
        if (digest_size != 32 || received != sizeof(digest_size))
            throw std::runtime_error("SHA-256 provider failed");
        object_.resize(object_size);
        require_crypto(BCryptCreateHash(algorithm_.handle, &hash_.handle, object_.data(),
                                       object_size, nullptr, 0, 0));
#else
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("SHA-256 provider failed");
#endif
    }
    void update(std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
        while (!bytes.empty()) {
            const auto length = static_cast<ULONG>(std::min<std::size_t>(bytes.size(),
                std::numeric_limits<ULONG>::max()));
            require_crypto(BCryptHashData(hash_.handle, const_cast<PUCHAR>(bytes.data()), length, 0));
            bytes = bytes.subspan(length);
        }
#else
        if (!bytes.empty() && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1)
            throw std::runtime_error("SHA-256 provider failed");
#endif
    }
    std::string finish() {
        std::array<std::uint8_t, 32> bytes{};
#ifdef _WIN32
        require_crypto(BCryptFinishHash(hash_.handle, bytes.data(), static_cast<ULONG>(bytes.size()), 0));
#else
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(context_.get(), bytes.data(), &length) != 1 || length != bytes.size())
            throw std::runtime_error("SHA-256 provider failed");
#endif
        constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            hex.push_back(digits[byte >> 4U]);
            hex.push_back(digits[byte & 15U]);
        }
        return hex;
    }
private:
#ifdef _WIN32
    Algorithm algorithm_;
    std::vector<std::uint8_t> object_;
    Hash hash_;
#else
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_{EVP_MD_CTX_new(), EVP_MD_CTX_free};
#endif
};
}

namespace hawken {
std::string sha256(std::span<const std::uint8_t> bytes) {
    Digest digest;
    digest.update(bytes);
    return digest.finish();
}
std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file for SHA-256");
    Digest digest;
    std::array<std::uint8_t, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) digest.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    }
    if (!input.eof()) throw std::runtime_error("Cannot read file for SHA-256");
    return digest.finish();
}
}
