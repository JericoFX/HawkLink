#include "hawken/server_login_handshake.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace login = hawken::control::login;
namespace post = hawken::control::posthello;
using hawken::control::Endian;
using hawken::control::NegotiatedHello;

// Literal hex transcription only; never calls any codec/formatter under test.
std::vector<std::uint8_t> bytes(std::string_view hex) {
    const auto digit=[](char c) -> unsigned {
        if (c>='0' && c<='9') return static_cast<unsigned>(c-'0');
        if (c>='a' && c<='f') return static_cast<unsigned>(c-'a')+10u;
        throw std::runtime_error("invalid public fixture hex");
    };
    if (hex.size()%2u) throw std::runtime_error("odd public fixture hex");
    std::vector<std::uint8_t> out;
    for (std::size_t i=0;i<hex.size();i+=2u)
        out.push_back(static_cast<std::uint8_t>((digit(hex[i])<<4u)|digit(hex[i+1u])));
    return out;
}
login::Config config(Endian endian=Endian::little) {
    return {endian,{128,64},0x1a2b3c4du,30000};
}
constexpr NegotiatedHello hello{11,0xfedcba98u};
const auto challenge_le=bytes("030b00000009000000314132423343344400");
const auto challenge_be=bytes("030000000b00000009314132423343344400");
const auto login_le=bytes("05030000005200fffdffffff000100d80000040302017856341200000080ffffffff");
const auto login_be=bytes("05000000035200fffffffffd000100d80000010203041234567880000000ffffffff");
const auto zero_login=bytes("050000000000000000040302017856341200000080ffffffff");
const post::Login raw_login{
    {post::StringForm::narrow,std::u16string{u'R',u'\0',u'\u00ff'}},
    {post::StringForm::wide,std::u16string{u'\u0100',static_cast<char16_t>(0xd800u),u'\0'}},
    {0x01020304u,0x12345678u,0x80000000u,0xffffffffu}
};
template<class Action>
void expect_error(Action action,login::ErrorCode expected) {
    try { action(); }
    catch (const login::Error& error) {
        CHECK(error.code()==expected);
        CHECK(std::string(error.what())=="Server login handshake operation could not be completed.");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed ServerLoginHandshake Error"); return; }
    FAIL_CHECK("server login handshake accepted rejected operation");
}
void ready(login::ServerLoginHandshake& machine,Endian endian=Endian::little) {
    const auto step=machine.begin(hello);
    REQUIRE(step.reply==(endian==Endian::little ? challenge_le : challenge_be));
    CHECK_FALSE(step.candidate.has_value());
    CHECK_FALSE(step.closed);
    CHECK(machine.snapshot()==login::Snapshot{login::State::awaiting_login,hello.caps,std::nullopt});
}
void unchanged_reject(login::ServerLoginHandshake& machine,const std::vector<std::uint8_t>& archive,
                      login::ErrorCode code) {
    const auto before=machine.snapshot();
    expect_error([&]{ (void)machine.receive(archive); },code);
    CHECK(machine.snapshot()==before);
}
}

// Independent byte layouts: Challenge is opcode3|version32|count32=9|
// eight uppercase ASCII digits|00 (18bytes). Login is opcode5|count+3|
// 52 00 ff|count-3|0100 d800 0000 in UTF16LE|four scalar u32 words.
// Only scalar words/counts swap with Endian; wide body never swaps.
TEST_CASE("ServerLogin begins with no implicit caps speed or admission") {
    login::ServerLoginHandshake machine(config());
    CHECK(machine.snapshot()==login::Snapshot{login::State::awaiting_hello,std::nullopt,std::nullopt});
}
TEST_CASE("ServerLogin Challenge little endian public literal") {
    login::ServerLoginHandshake machine(config());
    ready(machine);
}
TEST_CASE("ServerLogin Challenge big endian public literal") {
    login::ServerLoginHandshake machine(config(Endian::big));
    ready(machine,Endian::big);
}
TEST_CASE("ServerLogin challenge token formatting pads uppercase eight digits") {
    struct Fixture { std::uint32_t token; std::string_view archive; };
    const std::array fixtures{
        Fixture{0u,"030b00000009000000303030303030303000"},
        Fixture{1u,"030b00000009000000303030303030303100"},
        Fixture{0xabu,"030b00000009000000303030303030414200"},
        Fixture{0xffffffffu,"030b00000009000000464646464646464600"}
    };
    for (const auto& f:fixtures) {
        auto c=config(); c.challenge_token=f.token;
        login::ServerLoginHandshake machine(c);
        CHECK(machine.begin(hello).reply==bytes(f.archive));
        CHECK(machine.snapshot().state==login::State::awaiting_login);
    }
}
TEST_CASE("ServerLogin signed negotiated version preserves all scalar bits") {
    struct Fixture { std::int32_t version; std::string_view archive; };
    const std::array fixtures{
        Fixture{0,"030000000009000000314132423343344400"},
        Fixture{std::numeric_limits<std::int32_t>::max(),"03ffffff7f09000000314132423343344400"},
        Fixture{std::numeric_limits<std::int32_t>::min(),"030000008009000000314132423343344400"},
        Fixture{-1,"03ffffffff09000000314132423343344400"}
    };
    for (const auto& f:fixtures) {
        login::ServerLoginHandshake machine(config());
        CHECK(machine.begin({f.version,0}).reply==bytes(f.archive));
        CHECK(machine.snapshot().caps==std::optional<std::uint32_t>{0u});
    }
}
TEST_CASE("ServerLogin retains all caps including zero and high bits") {
    for (auto caps:std::array<std::uint32_t,3>{0u,0x80000000u,0xffffffffu}) {
        login::ServerLoginHandshake machine(config());
        (void)machine.begin({11,caps});
        CHECK(machine.snapshot().caps==std::optional<std::uint32_t>{caps});
        CHECK_FALSE(machine.snapshot().net_speed.has_value());
    }
}
TEST_CASE("ServerLogin validates explicit configuration without native defaults") {
    for (auto speed:std::array<std::int32_t,3>{1799,-1,std::numeric_limits<std::int32_t>::min()}) {
        auto c=config(); c.driver_max_speed=speed;
        expect_error([&]{ login::ServerLoginHandshake machine(c); },login::ErrorCode::invalid_configuration);
    }
    auto c=config(); c.endian=static_cast<Endian>(99);
    expect_error([&]{ login::ServerLoginHandshake machine(c); },login::ErrorCode::invalid_configuration);
}
TEST_CASE("ServerLogin rejects zero and unrepresentable codec limits") {
    for (auto limits:std::array<post::Limits,3>{{{0,64},{128,0},
        {128,static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1u}}}) {
        auto c=config(); c.limits=limits;
        expect_error([&]{ login::ServerLoginHandshake machine(c); },login::ErrorCode::invalid_configuration);
    }
}
TEST_CASE("ServerLogin begin accepts exact18byte nineunit budgets") {
    auto c=config(); c.limits={18,9};
    login::ServerLoginHandshake machine(c);
    ready(machine);
}
TEST_CASE("ServerLogin begin budget failures preserve awaiting hello") {
    for (auto limits:std::array<post::Limits,2>{{{17,9},{18,8}}}) {
        auto c=config(); c.limits=limits;
        login::ServerLoginHandshake machine(c);
        const auto before=machine.snapshot();
        expect_error([&]{ (void)machine.begin(hello); },login::ErrorCode::resource_limit);
        CHECK(machine.snapshot()==before);
    }
}
TEST_CASE("ServerLogin receiving before begin is own strict ordering policy") {
    login::ServerLoginHandshake machine(config());
    unchanged_reject(machine,bytes("0408070000"),login::ErrorCode::unexpected_message);
    unchanged_reject(machine,zero_login,login::ErrorCode::unexpected_message);
}
TEST_CASE("ServerLogin duplicate begin preserves original token caps and state") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    const auto before=machine.snapshot();
    expect_error([&]{ (void)machine.begin({-1,0}); },login::ErrorCode::unexpected_message);
    CHECK(machine.snapshot()==before);
}
TEST_CASE("ServerLogin signed little endian NetSpeed clamp native boundaries") {
    struct Fixture { std::string_view archive; std::int32_t speed; };
    const std::array fixtures{
        Fixture{"0400000000",1800},Fixture{"0407070000",1800},Fixture{"0408070000",1800},
        Fixture{"0430750000",30000},Fixture{"0431750000",30000},
        Fixture{"04ffffff7f",30000},Fixture{"0400000080",1800},Fixture{"04ffffffff",1800}
    };
    for (const auto& f:fixtures) {
        login::ServerLoginHandshake machine(config()); ready(machine);
        const auto step=machine.receive(bytes(f.archive));
        CHECK(step==login::Step{});
        CHECK(machine.snapshot()==login::Snapshot{login::State::awaiting_login,hello.caps,f.speed});
    }
}
TEST_CASE("ServerLogin big endian NetSpeed treats high bit as signed") {
    login::ServerLoginHandshake machine(config(Endian::big)); ready(machine,Endian::big);
    CHECK(machine.receive(bytes("0480000000"))==login::Step{});
    CHECK(machine.snapshot().net_speed==std::optional<std::int32_t>{1800});
    CHECK(machine.receive(bytes("0400001b58"))==login::Step{});
    CHECK(machine.snapshot().net_speed==std::optional<std::int32_t>{7000});
}
TEST_CASE("ServerLogin NetSpeed can repeat and driver minimum1800 is valid") {
    auto c=config(); c.driver_max_speed=1800;
    login::ServerLoginHandshake machine(c); ready(machine);
    CHECK(machine.receive(bytes("04ffffff7f"))==login::Step{});
    CHECK(machine.snapshot().net_speed==std::optional<std::int32_t>{1800});
    CHECK(machine.receive(bytes("0400000000"))==login::Step{});
    CHECK(machine.snapshot().net_speed==std::optional<std::int32_t>{1800});
    CHECK(machine.snapshot().state==login::State::awaiting_login);
}
TEST_CASE("ServerLogin Login does not require preceding NetSpeed") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    const auto step=machine.receive(zero_login);
    REQUIRE(step.candidate.has_value());
    CHECK(step.candidate->response==post::RawString{post::StringForm::zero,{}});
    CHECK(step.candidate->url==post::RawString{post::StringForm::zero,{}});
    CHECK(step.candidate->unique_id==raw_login.unique_id);
    CHECK(step.reply.empty()); CHECK_FALSE(step.closed);
    CHECK(machine.snapshot()==login::Snapshot{login::State::login_received,hello.caps,std::nullopt});
}
TEST_CASE("ServerLogin raw little endian Login preserves units and modernID") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    (void)machine.receive(bytes("04581b0000"));
    const auto step=machine.receive(login_le);
    REQUIRE(step.candidate.has_value()); CHECK(*step.candidate==raw_login);
    CHECK(step.reply.empty()); CHECK_FALSE(step.closed);
    CHECK(machine.snapshot()==login::Snapshot{login::State::login_received,hello.caps,7000});
}
TEST_CASE("ServerLogin raw big endian Login keeps wide body UTF16LE") {
    login::ServerLoginHandshake machine(config(Endian::big)); ready(machine,Endian::big);
    const auto step=machine.receive(login_be);
    REQUIRE(step.candidate.has_value()); CHECK(*step.candidate==raw_login);
    CHECK(machine.snapshot().state==login::State::login_received);
}
TEST_CASE("ServerLogin oneunit nonNUL FString forms remain raw") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    const auto step=machine.receive(bytes("0501000000ffffffffff000100000000000000000000000000000000"));
    REQUIRE(step.candidate.has_value());
    CHECK(step.candidate->response==post::RawString{post::StringForm::narrow,u"\u00ff"});
    CHECK(step.candidate->url==post::RawString{post::StringForm::wide,u"\u0100"});
}
TEST_CASE("ServerLogin output candidate is caller owned without machine retention") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    auto step=machine.receive(login_le); REQUIRE(step.candidate.has_value());
    const auto before=machine.snapshot();
    step.candidate->response.units.clear(); step.candidate->unique_id.fill(0);
    CHECK(machine.snapshot()==before);
    unchanged_reject(machine,login_le,login::ErrorCode::unexpected_message);
}
TEST_CASE("ServerLogin after Login rejects all messages and duplicate begin") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    const auto step=machine.receive(zero_login); REQUIRE(step.candidate.has_value());
    unchanged_reject(machine,bytes("0408070000"),login::ErrorCode::unexpected_message);
    unchanged_reject(machine,bytes("0600000000"),login::ErrorCode::unexpected_message);
    const auto before=machine.snapshot();
    expect_error([&]{ (void)machine.begin(hello); },login::ErrorCode::unexpected_message);
    CHECK(machine.snapshot()==before);
}
TEST_CASE("ServerLogin peer Failure closes without reflecting raw error text") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    (void)machine.receive(bytes("04581b0000"));
    const auto step=machine.receive(bytes("0604000000500049ff"));
    CHECK(step.reply.empty()); CHECK_FALSE(step.candidate.has_value()); CHECK(step.closed);
    CHECK(machine.snapshot()==login::Snapshot{login::State::closed,hello.caps,7000});
}
TEST_CASE("ServerLogin closed machine has fixed terminal errors") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    REQUIRE(machine.receive(bytes("0600000000")).closed);
    unchanged_reject(machine,zero_login,login::ErrorCode::terminal_state);
    unchanged_reject(machine,{},login::ErrorCode::terminal_state);
    const auto before=machine.snapshot();
    expect_error([&]{ (void)machine.begin(hello); },login::ErrorCode::terminal_state);
    CHECK(machine.snapshot()==before);
}
TEST_CASE("ServerLogin server only decoded opcodes are rejected unchanged") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    for (auto hex:std::array<std::string_view,3>{"010000000000000000","030000000000000000",
        "070000000000000000000000000000000000000000000000000000000000000000000000000000000000"})
        unchanged_reject(machine,bytes(hex),login::ErrorCode::unexpected_message);
}
TEST_CASE("ServerLogin unsupported Hello and unknown opcodes are invalid archives") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    for (auto hex:std::array<std::string_view,3>{"000a0000001400000000000000","08","ff"})
        unchanged_reject(machine,bytes(hex),login::ErrorCode::invalid_archive);
}
TEST_CASE("ServerLogin complete PackageAck is not login or admission") {
    // Recovered opcode8 plus five raw u32 words, all zero: valid in either
    // endian. A newly supported codec message must not advance login state.
    const auto ack=bytes("080000000000000000000000000000000000000000");
    for (const auto endian: {Endian::little,Endian::big}) {
        login::ServerLoginHandshake machine(config(endian));
        ready(machine,endian);
        unchanged_reject(machine,ack,login::ErrorCode::unexpected_message);
        const auto accepted=machine.receive(zero_login);
        REQUIRE(accepted.candidate.has_value());
        unchanged_reject(machine,ack,login::ErrorCode::unexpected_message);
    }
}
TEST_CASE("ServerLogin every truncated NetSpeed preserves previous speed") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    (void)machine.receive(bytes("04581b0000"));
    const auto complete=bytes("0408070000");
    for (std::size_t length=0;length<complete.size();++length)
        unchanged_reject(machine,{complete.begin(),complete.begin()+static_cast<std::ptrdiff_t>(length)},
                         login::ErrorCode::invalid_archive);
    CHECK(machine.snapshot().net_speed==std::optional<std::int32_t>{7000});
}
TEST_CASE("ServerLogin truncated modern identity and INT32MIN count are invalid") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    unchanged_reject(machine,bytes("0500000000000000000403020178563412"),login::ErrorCode::invalid_archive);
    auto short_id=zero_login; short_id.pop_back();
    unchanged_reject(machine,short_id,login::ErrorCode::invalid_archive);
    unchanged_reject(machine,bytes("0500000080"),login::ErrorCode::invalid_archive);
}
TEST_CASE("ServerLogin exact archive budget includes full Login identity") {
    auto c=config(); c.limits={33,64};
    login::ServerLoginHandshake machine(c); ready(machine);
    unchanged_reject(machine,login_le,login::ErrorCode::resource_limit);
    CHECK_FALSE(machine.snapshot().net_speed.has_value());
    c.limits.max_archive_bytes=34;
    login::ServerLoginHandshake exact(c); ready(exact);
    REQUIRE(exact.receive(login_le).candidate.has_value());
}
TEST_CASE("ServerLogin string count budget rejects before consuming missing body") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    unchanged_reject(machine,bytes("0541000000"),login::ErrorCode::resource_limit);
    unchanged_reject(machine,bytes("05bfffffff"),login::ErrorCode::resource_limit);
}
TEST_CASE("ServerLogin coalesced NetSpeed Login is rejected without partial speed") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    auto archive=bytes("04581b0000"); archive.insert(archive.end(),zero_login.begin(),zero_login.end());
    unchanged_reject(machine,archive,login::ErrorCode::invalid_archive);
    CHECK_FALSE(machine.snapshot().net_speed.has_value());
}
TEST_CASE("ServerLogin coalesced Login NetSpeed or suffix is rejected without candidate") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    auto archive=zero_login; const auto speed=bytes("0408070000");
    archive.insert(archive.end(),speed.begin(),speed.end());
    unchanged_reject(machine,archive,login::ErrorCode::invalid_archive);
    archive=zero_login; archive.push_back(0);
    unchanged_reject(machine,archive,login::ErrorCode::invalid_archive);
}
TEST_CASE("ServerLogin malformed Failure never closes or emits peer text") {
    login::ServerLoginHandshake machine(config()); ready(machine);
    unchanged_reject(machine,bytes("06120000006e657665722d6563686f2d707269766174"),login::ErrorCode::invalid_archive);
    CHECK(machine.snapshot().state==login::State::awaiting_login);
}
TEST_CASE("ServerLogin error messages remain fixed across error categories") {
    for (auto code:std::array<login::ErrorCode,6>{login::ErrorCode::invalid_configuration,
        login::ErrorCode::invalid_archive,login::ErrorCode::unexpected_message,login::ErrorCode::resource_limit,
        login::ErrorCode::terminal_state,login::ErrorCode::not_implemented}) {
        const login::Error error(code);
        CHECK(error.code()==code);
        CHECK(std::string(error.what())=="Server login handshake operation could not be completed.");
    }
}

TEST_CASE("ServerLogin valid Join is rejected before admission without changing state") {
    const std::vector<std::uint8_t> join{0x09};
    for (const auto endian: {Endian::little,Endian::big}) {
        login::ServerLoginHandshake machine(config(endian));
        unchanged_reject(machine,join,login::ErrorCode::unexpected_message);
        ready(machine,endian);
        (void)machine.receive(endian == Endian::little ? bytes("04581b0000") : bytes("0400001b58"));
        unchanged_reject(machine,join,login::ErrorCode::unexpected_message);
        const auto accepted=machine.receive(endian == Endian::little ? login_le : login_be);
        REQUIRE(accepted.candidate.has_value());
        unchanged_reject(machine,join,login::ErrorCode::unexpected_message);
    }
}
