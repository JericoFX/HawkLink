#include "hawken/session_wire.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

namespace {
using namespace hawken::session;

const char* error_message(SessionErrorCode code) noexcept {
    switch (code) {
    case SessionErrorCode::invalid_role: return "invalid session role";
    case SessionErrorCode::invalid_counter: return "invalid session counter";
    case SessionErrorCode::invalid_payload: return "invalid session payload size";
    case SessionErrorCode::invalid_packet_size: return "invalid session packet size";
    case SessionErrorCode::session_mismatch: return "session identifier mismatch";
    case SessionErrorCode::wrong_direction: return "invalid session packet direction";
    case SessionErrorCode::replay: return "session packet is not newer";
    case SessionErrorCode::authentication_failed: return "session authentication failed";
    case SessionErrorCode::counter_exhausted: return "session counter exhausted";
    case SessionErrorCode::crypto_failure: return "session cryptographic provider failed";
    case SessionErrorCode::not_implemented: return "session codec unavailable";
    }
    return "session codec failed";
}

void validate_role(Role role) {
    if (role != Role::client && role != Role::server)
        throw SessionError(SessionErrorCode::invalid_role);
}

void erase_bytes(void* memory, std::size_t size) noexcept {
#ifdef _WIN32
    SecureZeroMemory(memory, size);
#else
    OPENSSL_cleanse(memory, size);
#endif
}

struct EraseOnExit {
    void* memory;
    std::size_t size;
    ~EraseOnExit() { erase_bytes(memory, size); }
    EraseOnExit(const EraseOnExit&) = delete;
    EraseOnExit& operator=(const EraseOnExit&) = delete;
    EraseOnExit(void* location, std::size_t length) noexcept : memory(location), size(length) {}
};

std::uint32_t read_le32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_le32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (unsigned int i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint64_t read_le64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}

void write_le64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (unsigned int i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void quarter_round(std::uint32_t& a, std::uint32_t& b,
                   std::uint32_t& c, std::uint32_t& d) noexcept {
    a += b; d = std::rotl(d ^ a, 16);
    c += d; b = std::rotl(b ^ c, 12);
    a += b; d = std::rotl(d ^ a, 8);
    c += d; b = std::rotl(b ^ c, 7);
}

// Original ChaCha20: state[12:13]=blockcounter64, [14:15]=nonce64.
// https://doc.libsodium.org/advanced/stream_ciphers/chacha20
// Native df5e90/df5400/df5160; intentionally distinct from IETF nonce96.
void transform(const SessionKey& key, std::uint64_t nonce,
               std::span<const std::uint8_t> input,
               std::span<std::uint8_t> output) noexcept {
    std::array<std::uint32_t, 16> state{};
    std::array<std::uint32_t, 16> working{};
    std::array<std::uint8_t, 64> stream{};
    EraseOnExit erase_state(state.data(), sizeof(state));
    EraseOnExit erase_working(working.data(), sizeof(working));
    EraseOnExit erase_stream(stream.data(), sizeof(stream));
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (std::size_t i = 0; i < key.size() / 4; ++i)
        state[4 + i] = read_le32(key.data() + i * 4);
    state[14] = static_cast<std::uint32_t>(nonce);
    state[15] = static_cast<std::uint32_t>(nonce >> 32);
    for (std::size_t position = 0; position < input.size();) {
        working = state;
        for (int round = 0; round < 10; ++round) {
            quarter_round(working[0], working[4], working[8], working[12]);
            quarter_round(working[1], working[5], working[9], working[13]);
            quarter_round(working[2], working[6], working[10], working[14]);
            quarter_round(working[3], working[7], working[11], working[15]);
            quarter_round(working[0], working[5], working[10], working[15]);
            quarter_round(working[1], working[6], working[11], working[12]);
            quarter_round(working[2], working[7], working[8], working[13]);
            quarter_round(working[3], working[4], working[9], working[14]);
        }
        for (std::size_t i = 0; i < state.size(); ++i)
            write_le32(stream.data() + 4 * i, working[i] + state[i]);
        const auto count = std::min(stream.size(), input.size() - position);
        for (std::size_t i = 0; i < count; ++i)
            output[position + i] = static_cast<std::uint8_t>(input[position + i] ^ stream[i]);
        position += count;
        // The bounded payload uses <=1023 blocks, far below counter64 overflow.
        if (++state[12] == 0) ++state[13];
    }
}

#ifdef _WIN32
void require_crypto(NTSTATUS status) {
    if (status < 0) throw SessionError(SessionErrorCode::crypto_failure);
}
struct CngHmac {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    ~CngHmac() {
        if (hash) BCryptDestroyHash(hash);
        if (!object.empty()) erase_bytes(object.data(), object.size());
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CngHmac() = default;
    CngHmac(const CngHmac&) = delete;
    CngHmac& operator=(const CngHmac&) = delete;
};
#endif

std::array<std::uint8_t, 32> authenticate(const SessionKey& key,
                                       std::span<const std::uint8_t> input) {
    std::array<std::uint8_t, 64> digest{};
    EraseOnExit erase_digest(digest.data(), digest.size());
#ifdef _WIN32
    // HMAC flag + SHA512 produces the full digest before truncation, not SHA512/256.
    // https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptopenalgorithmprovider
    // https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash
    CngHmac context;
    require_crypto(BCryptOpenAlgorithmProvider(&context.algorithm,
        BCRYPT_SHA512_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
    DWORD object_size = 0, digest_size = 0, received = 0;
    require_crypto(BCryptGetProperty(context.algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0));
    if (received != sizeof(object_size) || object_size == 0)
        throw SessionError(SessionErrorCode::crypto_failure);
    require_crypto(BCryptGetProperty(context.algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size), &received, 0));
    if (received != sizeof(digest_size) || digest_size != digest.size())
        throw SessionError(SessionErrorCode::crypto_failure);
    context.object.resize(object_size);
    require_crypto(BCryptCreateHash(context.algorithm, &context.hash, context.object.data(),
        object_size, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0));
    require_crypto(BCryptHashData(context.hash, const_cast<PUCHAR>(input.data()),
        static_cast<ULONG>(input.size()), 0));
    require_crypto(BCryptFinishHash(context.hash, digest.data(),
        static_cast<ULONG>(digest.size()), 0));
#else
    // OpenSSL3 one-shot provider MAC. The supplied output buffer is always 64B.
    // https://docs.openssl.org/3.0/man3/EVP_MAC/
    std::size_t length = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA512", nullptr, key.data(), key.size(),
        input.data(), input.size(), digest.data(), digest.size(), &length) == nullptr
        || length != digest.size())
        throw SessionError(SessionErrorCode::crypto_failure);
#endif
    std::array<std::uint8_t, 32> tag{};
    std::copy_n(digest.begin(), tag.size(), tag.begin());
    return tag;
}

bool equal_tag(const std::array<std::uint8_t, 32>& expected,
               std::span<const std::uint8_t> actual) noexcept {
    // Read all 32 bytes without an early mismatch exit, as in native df51d0.
    // Volatile accumulation keeps each comparison; this is not a timing audit.
    volatile std::uint32_t difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        difference = difference | static_cast<std::uint32_t>(expected[i] ^ actual[i]);
    return difference == 0;
}
}

namespace hawken::session {
SessionError::SessionError(SessionErrorCode code)
    : std::runtime_error(error_message(code)), code_(code) {}
SessionErrorCode SessionError::code() const noexcept { return code_; }

std::uint64_t next_send_counter(Role role, std::uint64_t current) {
    validate_role(role);
    const auto high = (current & direction_bit) != 0;
    if (high != (role == Role::server))
        throw SessionError(SessionErrorCode::invalid_counter);
    const auto maximum = role == Role::client
        ? direction_bit - 1 : std::numeric_limits<std::uint64_t>::max();
    if (current == maximum) throw SessionError(SessionErrorCode::counter_exhausted);
    return current + 1;
}
Context::Context(Role role, const SessionId& id, const SessionKey& key) : role_(role) {
    validate_role(role);
    id_ = id;
    key_ = key;
    last_sent_ = role == Role::server ? direction_bit : 0;
}
Context::~Context() { erase_bytes(key_.data(), key_.size()); }

std::vector<std::uint8_t> Context::encode(std::span<const std::uint8_t> payload) {
    if (payload.empty() || payload.size() > max_payload_bytes)
        throw SessionError(SessionErrorCode::invalid_payload);
    // Reserve before allocating/encrypting/MAC; a failure cannot reuse the nonce.
    last_sent_ = next_send_counter(role_, last_sent_);
    std::vector<std::uint8_t> packet(payload.size() + wrapper_bytes);
    std::copy(id_.begin(), id_.end(), packet.begin());
    write_le64(packet.data() + 8, last_sent_);
    transform(key_, last_sent_, payload, std::span(packet).subspan(16, payload.size()));
    const auto tag = authenticate(key_, std::span(packet).first(16 + payload.size()));
    std::copy(tag.begin(), tag.end(), packet.begin() + 16 + payload.size());
    return packet;
}

std::vector<std::uint8_t> Context::decode(std::span<const std::uint8_t> packet) {
    if (packet.size() <= wrapper_bytes || packet.size() > max_packet_bytes)
        throw SessionError(SessionErrorCode::invalid_packet_size);
    if (!std::equal(id_.begin(), id_.end(), packet.begin()))
        throw SessionError(SessionErrorCode::session_mismatch);
    const auto counter = read_le64(packet.data() + 8);
    const auto high = (counter & direction_bit) != 0;
    if (high != (role_ == Role::client))
        throw SessionError(SessionErrorCode::wrong_direction);
    if (counter <= last_received_) throw SessionError(SessionErrorCode::replay);
    const auto authenticated_bytes = packet.size() - 32;
    const auto tag = authenticate(key_, packet.first(authenticated_bytes));
    if (!equal_tag(tag, packet.last(32)))
        throw SessionError(SessionErrorCode::authentication_failed);
    std::vector<std::uint8_t> payload(packet.size() - wrapper_bytes);
    transform(key_, counter, packet.subspan(16, payload.size()), payload);
    last_received_ = counter;
    return payload;
}
std::uint64_t Context::last_sent() const noexcept { return last_sent_; }
std::uint64_t Context::last_received() const noexcept { return last_received_; }
}
