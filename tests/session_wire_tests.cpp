#include "hawken/session_wire.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
using namespace hawken::session;
static_assert(!std::is_copy_constructible_v<Context>);
static_assert(!std::is_move_constructible_v<Context>);
static_assert(!std::is_copy_assignable_v<Context>);
static_assert(!std::is_move_assignable_v<Context>);
const SessionId public_id{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

SessionKey public_key() {
    SessionKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    return key;
}
std::vector<std::uint8_t> public_payload() {
    std::vector<std::uint8_t> result(80);
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<std::uint8_t>(i);
    return result;
}
std::vector<std::uint8_t> hex(std::string_view text) {
    auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::logic_error("invalid public test vector");
    };
    if (text.size() % 2 != 0) throw std::logic_error("invalid public test vector");
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return result;
}
// Independent oracle: OpenSSL 3.5.7 CLI, not the codec/writer under test.
// Evidence: implementation/evidence/session-wire-oracle/{counter-1,
// counter-8000000000000001}/vector.json; all encrypt/HMAC exits 0.
// Key00..1f, ID0011223344556677, plaintext00..4f, blockcounter0.
// IV16 = zero64 || nonceLE64. MAC covers header16+cipher80, SHA512 first32.
// Public synthetic credentials only; this is not a Hawken acceptance fixture.
std::vector<std::uint8_t> oracle_client_packet() {
    // SHA256 98eb2d52d222f24a4989a73b42bab97c956e0af73a522a231b2aa8a98dd0619b
    return hex(
        "00112233445566770100000000000000"
        "2fa5f3015485888eaa5b3bee03d260ef61d74d32fb8bf86f542637921a07f68a"
        "3ecafc7a2002a2dd0f369ff51ba443803035d6c74930c804842d274f5339c436"
        "df16b11cf16fe95f61ffe4171ed8d7c8"
        "22dea0b37c4448207f05be5c700c4af730915f28f883263d4a6fa64ed06f944d");
}
std::vector<std::uint8_t> oracle_server_packet() {
    // SHA256 534f346100a34260ec74c6bcf0efaf6940651193212e2c9574795c34ae0225d8
    return hex(
        "00112233445566770100000000000080"
        "5ab3f414f6e8090270b0f3519e77f6610452eb206dd3c0ab1b039b02cbbc01be"
        "c63355be35ec175078c6fe9a934e671b1f6226c6dc30c3767663e4119ee31daa"
        "b7a49672ed4d8895dd64eea17cd9d5b7"
        "b67791d042c1ee654d4ccebfa031e00e79df3810b38c6f6c353589e6a94cc1bc");
}
template<class Action>
void expect_error(Action action, SessionErrorCode expected) {
    try { action(); }
    catch (const SessionError& error) {
        CHECK(error.code() == expected);
        CHECK(std::string(error.what()).find("0011223344556677") == std::string::npos);
        CHECK(std::string(error.what()).find("never-echo-this") == std::string::npos);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected sanitized SessionError"); return; }
    FAIL_CHECK("session codec accepted rejected input");
}
}

TEST_CASE("Session counters start in separate direction halves") {
    Context client(Role::client, public_id, public_key());
    Context server(Role::server, public_id, public_key());
    CHECK(client.last_sent() == 0);
    CHECK(client.last_received() == 0);
    CHECK(server.last_sent() == direction_bit);
    CHECK(server.last_received() == 0);
    CHECK(next_send_counter(Role::client, 0) == 1);
    CHECK(next_send_counter(Role::server, direction_bit) == direction_bit + 1);
}

TEST_CASE("Session counter transition rejects exhaustion without wrapping") {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(next_send_counter(Role::client, direction_bit - 2) == direction_bit - 1);
    CHECK(next_send_counter(Role::server, maximum - 1) == maximum);
    expect_error([&] { next_send_counter(Role::client, direction_bit - 1); }, SessionErrorCode::counter_exhausted);
    expect_error([&] { next_send_counter(Role::server, maximum); }, SessionErrorCode::counter_exhausted);
    expect_error([&] { next_send_counter(Role::client, direction_bit); }, SessionErrorCode::invalid_counter);
    expect_error([&] { next_send_counter(Role::server, 0); }, SessionErrorCode::invalid_counter);
}

TEST_CASE("Session codec rejects invalid roles") {
    const auto invalid = static_cast<Role>(99);
    expect_error([&] { Context context(invalid, public_id, public_key()); }, SessionErrorCode::invalid_role);
    expect_error([&] { next_send_counter(invalid, 0); }, SessionErrorCode::invalid_role);
}

TEST_CASE("Session cipher matches published libsodium original nonce64 vector") {
    // Primary libsodium 1.0.20 test/default/chacha20.c tests[3], chacha20.exp
    // fourth output: zero key, nonce 0100000000000000, blockcounter0, 160 zero bytes.
    // https://github.com/jedisct1/libsodium/blob/1.0.20/test/default/chacha20.c
    // https://github.com/jedisct1/libsodium/blob/1.0.20/test/default/chacha20.exp
    const auto expected = hex(
        "ef3fdfd6c61578fbf5cf35bd3dd33b8009631634d21e42ac33960bd138e50d321"
        "11e4caf237ee53ca8ad6426194a88545ddc497a0b466e7d6bbdb0041b2f586b5"
        "305e5e44aff19b235936144675efbe4409eb7e8e5f1430f5f5836aeb49bb5328"
        "b017c4b9dc11f8a03863fa803dc71d5726b2b6b31aa32708afe5af1d6b690584"
        "d58792b271e5fdb92c486051c48b79a4d48a109bb2d0477956e74c25e93c3c2");
    Context client(Role::client, public_id, SessionKey{});
    const auto packet = client.encode(std::vector<std::uint8_t>(160, 0));
    REQUIRE(packet.size() == 160 + wrapper_bytes);
    CHECK(std::equal(expected.begin(), expected.end(), packet.begin() + 16));
    CHECK(packet[8] == 1);
    for (std::size_t i = 9; i < 16; ++i) CHECK(packet[i] == 0);
}

TEST_CASE("Session client emission matches independent complete packet oracle") {
    Context client(Role::client, public_id, public_key());
    CHECK(client.encode(public_payload()) == oracle_client_packet());
    CHECK(client.last_sent() == 1);
    // Reciprocal server decoding is an explicit codec policy, not server RE.
    Context server(Role::server, public_id, public_key());
    CHECK(server.decode(oracle_client_packet()) == public_payload());
    CHECK(server.last_received() == 1);
}

TEST_CASE("Session server emission and native client receive policy match oracle") {
    Context server(Role::server, public_id, public_key());
    CHECK(server.encode(public_payload()) == oracle_server_packet());
    CHECK(server.last_sent() == direction_bit + 1);
    Context client(Role::client, public_id, public_key());
    CHECK(client.decode(oracle_server_packet()) == public_payload());
    CHECK(client.last_received() == direction_bit + 1);
}

TEST_CASE("Session maximum covers total UDP bytes and positive payload") {
    Context client(Role::client, public_id, public_key());
    Context server(Role::server, public_id, public_key());
    expect_error([&] { client.encode({}); }, SessionErrorCode::invalid_payload);
    CHECK(client.last_sent() == 0);
    expect_error([&] { client.encode(std::vector<std::uint8_t>(max_payload_bytes + 1)); }, SessionErrorCode::invalid_payload);
    CHECK(client.last_sent() == 0);
    const std::vector<std::uint8_t> payload(max_payload_bytes, 0xa5);
    const auto packet = client.encode(payload);
    REQUIRE(packet.size() == max_packet_bytes);
    CHECK(server.decode(packet) == payload);
    expect_error([&] { server.decode(std::vector<std::uint8_t>(max_packet_bytes + 1)); }, SessionErrorCode::invalid_packet_size);
    CHECK(server.last_received() == 1);
}

TEST_CASE("Session rejects every packet length through empty payload wrapper") {
    Context client(Role::client, public_id, public_key());
    for (std::size_t length = 0; length <= wrapper_bytes; ++length)
        expect_error([&] { client.decode(std::vector<std::uint8_t>(length)); }, SessionErrorCode::invalid_packet_size);
    CHECK(client.last_received() == 0);
}

TEST_CASE("Session first outgoing header encodes ID and LE64 counter") {
    Context client(Role::client, public_id, public_key());
    Context server(Role::server, public_id, public_key());
    const auto c = client.encode(public_payload());
    const auto s = server.encode(public_payload());
    REQUIRE(c.size() == public_payload().size() + wrapper_bytes);
    REQUIRE(s.size() == public_payload().size() + wrapper_bytes);
    const auto expected_c = hex("00112233445566770100000000000000");
    const auto expected_s = hex("00112233445566770100000000000080");
    CHECK(std::equal(expected_c.begin(), expected_c.end(), c.begin()));
    CHECK(std::equal(expected_s.begin(), expected_s.end(), s.begin()));
    CHECK(client.last_sent() == 1);
    CHECK(server.last_sent() == direction_bit + 1);
}

TEST_CASE("Session wrong ID and opposite direction cannot advance receive state") {
    Context sender(Role::server, public_id, public_key());
    Context client(Role::client, public_id, public_key());
    auto packet = sender.encode(public_payload());
    packet[0] ^= 1;
    expect_error([&] { client.decode(packet); }, SessionErrorCode::session_mismatch);
    packet[0] ^= 1;
    packet[15] ^= 0x80;
    expect_error([&] { client.decode(packet); }, SessionErrorCode::wrong_direction);
    CHECK(client.last_received() == 0);
    Context low_sender(Role::client, public_id, public_key());
    Context low_receiver(Role::client, public_id, public_key());
    expect_error([&] { low_receiver.decode(low_sender.encode(public_payload())); }, SessionErrorCode::wrong_direction);
    Context high_receiver(Role::server, public_id, public_key());
    expect_error([&] { high_receiver.decode(sender.encode(public_payload())); }, SessionErrorCode::wrong_direction);
}

TEST_CASE("Session authenticates ciphertext and all 32 tag bytes before accepting") {
    const auto valid = oracle_server_packet();
    for (const auto offset : {std::size_t{16}, valid.size() - 33}) {
        Context client(Role::client, public_id, public_key());
        auto damaged = valid;
        damaged[offset] ^= 1;
        expect_error([&] { client.decode(damaged); }, SessionErrorCode::authentication_failed);
        CHECK(client.last_received() == 0);
        CHECK(client.decode(valid) == public_payload());
    }
    for (std::size_t index = 0; index < 32; ++index) {
        Context client(Role::client, public_id, public_key());
        auto damaged = valid;
        damaged[damaged.size() - 32 + index] ^= 0x80;
        expect_error([&] { client.decode(damaged); }, SessionErrorCode::authentication_failed);
        CHECK(client.last_received() == 0);
    }
}

TEST_CASE("Session forged greater counter and wrong key leave valid packet acceptable") {
    Context client(Role::client, public_id, public_key());
    const auto valid = oracle_server_packet();
    auto forged = valid;
    forged[8] = 2;
    expect_error([&] { client.decode(forged); }, SessionErrorCode::authentication_failed);
    CHECK(client.last_received() == 0);
    CHECK(client.decode(valid) == public_payload());
    auto other_key = public_key();
    other_key[31] ^= 1;
    Context wrong_key(Role::client, public_id, other_key);
    expect_error([&] { wrong_key.decode(valid); }, SessionErrorCode::authentication_failed);
    CHECK(wrong_key.last_received() == 0);
}

TEST_CASE("Session receive ordering is strict unsigned and direction-specific") {
    Context sender(Role::server, public_id, public_key());
    Context client(Role::client, public_id, public_key());
    const auto first = sender.encode(public_payload());
    const auto second = sender.encode(public_payload());
    const auto third = sender.encode(public_payload());
    CHECK(client.decode(third) == public_payload());
    CHECK(client.last_received() == direction_bit + 3);
    expect_error([&] { client.decode(first); }, SessionErrorCode::replay);
    expect_error([&] { client.decode(second); }, SessionErrorCode::replay);
    expect_error([&] { client.decode(third); }, SessionErrorCode::replay);
    CHECK(client.last_received() == direction_bit + 3);
    Context low_sender(Role::client, public_id, public_key());
    Context server(Role::server, public_id, public_key());
    const auto low = low_sender.encode(public_payload());
    CHECK(server.decode(low) == public_payload());
    CHECK(server.last_received() == 1);
    expect_error([&] { server.decode(low); }, SessionErrorCode::replay);
}

TEST_CASE("Session roundtrips block boundaries as supplementary state tests") {
    for (const std::size_t length : {1U, 63U, 64U, 65U, 127U, 128U, 129U}) {
        Context client(Role::client, public_id, public_key());
        Context server(Role::server, public_id, public_key());
        std::vector<std::uint8_t> payload(length, 0xab);
        const auto first = client.encode(payload);
        CHECK(server.decode(first) == payload);
        const auto second = client.encode(payload);
        CHECK(first != second);
        CHECK(server.decode(second) == payload);
        CHECK(client.last_sent() == 2);
        CHECK(server.last_received() == 2);
    }
}
