#include "hawken/posthello_control.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using hawken::control::Endian;
using namespace hawken::control::posthello;
constexpr Limits roomy{1024,64};

template<class Action>
void expect_error(Action action, ErrorCode expected) {
    try { action(); }
    catch (const Error& error) {
        CHECK(error.code() == expected);
        const std::string text=error.what();
        CHECK(text.find("never-echo-private") == std::string::npos);
        CHECK(text.find("userinfo-guid-secret") == std::string::npos);
        CHECK(text.find("https://private.invalid") == std::string::npos);
        // Fixed error text is part of the no-input-echo contract, independent
        // of the supplied identity, URL, raw units and malformed bytes.
        switch (expected) {
        case ErrorCode::invalid_endian: CHECK(text == "invalid post-Hello endian"); break;
        case ErrorCode::invalid_limits: CHECK(text == "invalid post-Hello limits"); break;
        case ErrorCode::invalid_message: CHECK(text == "invalid post-Hello message"); break;
        case ErrorCode::unsupported_opcode: CHECK(text == "unsupported post-Hello opcode"); break;
        case ErrorCode::truncated_archive: CHECK(text == "truncated post-Hello archive"); break;
        case ErrorCode::extra_bytes: CHECK(text == "extra post-Hello archive bytes"); break;
        case ErrorCode::resource_limit: CHECK(text == "post-Hello resource limit"); break;
        case ErrorCode::not_implemented: FAIL_CHECK("not_implemented is not a behavior expectation"); break;
        }
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed post-Hello Error"); return; }
    FAIL_CHECK("post-Hello codec accepted rejected input");
}

struct Fixture {
    Message message;
    std::vector<std::uint8_t> little;
    std::vector<std::uint8_t> big;
};

// Independently written byte literals, not expected values built by codec
// helpers. Each signed count is 4bytes; narrow body is one byte per unit;
// wide body is two LE bytes per unit even when scalar endian is big.
// Welcome: opcode1 | count0 | count-2 | units0100,0000 =13bytes.
// Challenge: opcode3 | version12345678 | count3 | units80,ff,00 =12bytes.
// Login: opcode5 | count2,R,00 | count0 | four IDwords =27bytes.
// Failure: opcode6 | count-4 | d800,0000,0058,dc00 =13bytes.
// Package: opcode7 | four identitywords | count0 | count2,N,00 |
// count-1,0100 | value0,value1 | count1,ff | flags81 =47bytes.
const std::array<Fixture,6> fixtures{{
    {Welcome{{StringForm::zero,{}},{StringForm::wide,{0x0100,0}}},
        {1,0,0,0,0,0xfe,0xff,0xff,0xff,0,1,0,0},
        {1,0,0,0,0,0xff,0xff,0xff,0xfe,0,1,0,0}},
    {Challenge{0x12345678u,{StringForm::narrow,{0x80,0xff,0}}},
        {3,0x78,0x56,0x34,0x12,3,0,0,0,0x80,0xff,0},
        {3,0x12,0x34,0x56,0x78,0,0,0,3,0x80,0xff,0}},
    {NetSpeed{0xfedcba98u},
        {4,0x98,0xba,0xdc,0xfe}, {4,0xfe,0xdc,0xba,0x98}},
    {Login{{StringForm::narrow,{u'R',0}},{StringForm::zero,{}},
        {0x11223344u,0x55667788u,0x99aabbccu,0xddeeff00u}},
        {5,2,0,0,0,0x52,0,0,0,0,0,0x44,0x33,0x22,0x11,
            0x88,0x77,0x66,0x55,0xcc,0xbb,0xaa,0x99,0,0xff,0xee,0xdd},
        {5,0,0,0,2,0x52,0,0,0,0,0,0x11,0x22,0x33,0x44,
            0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0}},
    {Failure{{StringForm::wide,{0xd800,0,u'X',0xdc00}}},
        {6,0xfc,0xff,0xff,0xff,0,0xd8,0,0,0x58,0,0,0xdc},
        {6,0xff,0xff,0xff,0xfc,0,0xd8,0,0,0x58,0,0,0xdc}},
    {Package{{1,0x80000000u,0x12345678u,0xffffffffu},
        {StringForm::zero,{}},{StringForm::narrow,{u'N',0}},
        {StringForm::wide,{0x0100}},0x01020304u,0xa0b0c0d0u,
        {StringForm::narrow,{0xff}},0x81},
        {7,1,0,0,0,0,0,0,0x80,0x78,0x56,0x34,0x12,0xff,0xff,0xff,0xff,
            0,0,0,0,2,0,0,0,0x4e,0,0xff,0xff,0xff,0xff,0,1,
            4,3,2,1,0xd0,0xc0,0xb0,0xa0,1,0,0,0,0xff,0x81},
        {7,0,0,0,1,0x80,0,0,0,0x12,0x34,0x56,0x78,0xff,0xff,0xff,0xff,
            0,0,0,0,0,0,0,2,0x4e,0,0xff,0xff,0xff,0xff,0,1,
            1,2,3,4,0xa0,0xb0,0xc0,0xd0,0,0,0,1,0xff,0x81}}
}};

void check_fixture(std::size_t i) {
    const auto& f=fixtures[i];
    CHECK(encode(f.message,Endian::little,roomy) == f.little);
    CHECK(encode(f.message,Endian::big,roomy) == f.big);
    CHECK(decode_exact(f.little,Endian::little,roomy) == f.message);
    CHECK(decode_exact(f.big,Endian::big,roomy) == f.message);
    const auto little=decode_prefix(f.little,Endian::little,roomy);
    CHECK(little.message == f.message);
    CHECK(little.consumed == f.little.size());
    const auto big=decode_prefix(f.big,Endian::big,roomy);
    CHECK(big.message == f.message);
    CHECK(big.consumed == f.big.size());
}
}

TEST_CASE("PostHello Welcome has two raw strings and no redirect field") { check_fixture(0); }
TEST_CASE("PostHello Challenge preserves version bits and narrow80ff units") { check_fixture(1); }
TEST_CASE("PostHello NetSpeed preserves all unsigned bits without a clamp") { check_fixture(2); }
TEST_CASE("PostHello Login modern ID is four individually ordered words") { check_fixture(3); }
TEST_CASE("PostHello Failure retains interior NUL and unpaired surrogate units") { check_fixture(4); }
TEST_CASE("PostHello Package preserves eleven neutral fields in their wire order") { check_fixture(5); }

TEST_CASE("PostHello prefix consumes one message despite a larger coalesced span") {
    const std::vector<std::uint8_t> bytes{4,0x78,0x56,0x34,0x12,6,0,0,0,0};
    const auto first=decode_prefix(bytes,Endian::little,Limits{5,1});
    CHECK(first.message == Message{NetSpeed{0x12345678u}});
    CHECK(first.consumed == 5);
    const auto second=decode_prefix(std::span(bytes).subspan(first.consumed),Endian::little,Limits{5,1});
    CHECK(second.message == Message{Failure{{StringForm::zero,{}}}});
    CHECK(second.consumed == 5);
}
TEST_CASE("PostHello prefix leaves unsupported suffix unread but exact rejects it") {
    const std::vector<std::uint8_t> bytes{4,0,0,0,0,0xff,0x80};
    const auto first=decode_prefix(bytes,Endian::little,Limits{5,1});
    CHECK(first.message == Message{NetSpeed{0}});
    CHECK(first.consumed == 5);
    expect_error([&]{decode_exact(bytes,Endian::little,Limits{5,1});},ErrorCode::extra_bytes);
}
TEST_CASE("PostHello exact rejects a second valid message") {
    const std::vector<std::uint8_t> bytes{6,0,0,0,0,4,0,0,0,0};
    expect_error([&]{decode_exact(bytes,Endian::little,roomy);},ErrorCode::extra_bytes);
}
TEST_CASE("PostHello zero plusone and minusone keep distinct raw representations") {
    const std::array<Fixture,5> values{{
        {Failure{{StringForm::zero,{}}},{6,0,0,0,0},{6,0,0,0,0}},
        {Failure{{StringForm::narrow,{0}}},{6,1,0,0,0,0},{6,0,0,0,1,0}},
        {Failure{{StringForm::wide,{0}}},{6,0xff,0xff,0xff,0xff,0,0},{6,0xff,0xff,0xff,0xff,0,0}},
        {Failure{{StringForm::narrow,{u'Z'}}},{6,1,0,0,0,0x5a},{6,0,0,0,1,0x5a}},
        {Failure{{StringForm::wide,{0x0100}}},{6,0xff,0xff,0xff,0xff,0,1},{6,0xff,0xff,0xff,0xff,0,1}}
    }};
    for (const auto& f:values) {
        CHECK(encode(f.message,Endian::little,roomy) == f.little);
        CHECK(encode(f.message,Endian::big,roomy) == f.big);
        CHECK(decode_exact(f.little,Endian::little,roomy) == f.message);
        CHECK(decode_exact(f.big,Endian::big,roomy) == f.message);
    }
}
TEST_CASE("PostHello wide body remains LE with opposite endian prefix") {
    const Message value=Failure{{StringForm::wide,{0x1234,0x0100}}};
    const std::vector<std::uint8_t> bytes{6,0xff,0xff,0xff,0xfe,0x34,0x12,0,1};
    CHECK(encode(value,Endian::big,roomy) == bytes);
    CHECK(decode_exact(bytes,Endian::big,roomy) == value);
}
TEST_CASE("PostHello narrow interior NUL does not truncate later units") {
    const Message value=Failure{{StringForm::narrow,{u'A',0,0xff,u'B'}}};
    const std::vector<std::uint8_t> bytes{6,4,0,0,0,0x41,0,0xff,0x42};
    CHECK(encode(value,Endian::little,roomy) == bytes);
    CHECK(decode_exact(bytes,Endian::little,roomy) == value);
}
TEST_CASE("PostHello scalar endian is explicit rather than guessed") {
    const std::vector<std::uint8_t> bytes{4,1,2,3,4};
    CHECK(decode_exact(bytes,Endian::little,roomy) == Message{NetSpeed{0x04030201}});
    CHECK(decode_exact(bytes,Endian::big,roomy) == Message{NetSpeed{0x01020304}});
}
TEST_CASE("PostHello invalid endian is a fixed typed error in every entry point") {
    const auto bad=static_cast<Endian>(99);
    const std::vector<std::uint8_t> bytes{4,0,0,0,0};
    expect_error([&]{encode(NetSpeed{0},bad,roomy);},ErrorCode::invalid_endian);
    expect_error([&]{decode_prefix(bytes,bad,roomy);},ErrorCode::invalid_endian);
    expect_error([&]{decode_exact(bytes,bad,roomy);},ErrorCode::invalid_endian);
}
TEST_CASE("PostHello zero byte or unit budget is invalid configuration") {
    const std::vector<std::uint8_t> bytes{4,0,0,0,0};
    for (const Limits limits: {Limits{0,1},Limits{5,0}}) {
        expect_error([&]{encode(NetSpeed{0},Endian::little,limits);},ErrorCode::invalid_limits);
        expect_error([&]{decode_prefix(bytes,Endian::little,limits);},ErrorCode::invalid_limits);
        expect_error([&]{decode_exact(bytes,Endian::little,limits);},ErrorCode::invalid_limits);
    }
}
TEST_CASE("PostHello unit budget cannot exceed positive signed32 count range") {
    const Limits limits{64,static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1};
    const std::vector<std::uint8_t> bytes{4,0,0,0,0};
    expect_error([&]{encode(NetSpeed{0},Endian::little,limits);},ErrorCode::invalid_limits);
    expect_error([&]{decode_prefix(bytes,Endian::little,limits);},ErrorCode::invalid_limits);
    expect_error([&]{decode_exact(bytes,Endian::little,limits);},ErrorCode::invalid_limits);
}
TEST_CASE("PostHello archive budget is exact including opcode and all count prefixes") {
    const Message value=Failure{{StringForm::narrow,{u'A'}}};
    const std::vector<std::uint8_t> bytes{6,1,0,0,0,0x41};
    CHECK(encode(value,Endian::little,Limits{6,1}) == bytes);
    CHECK(decode_exact(bytes,Endian::little,Limits{6,1}) == value);
    expect_error([&]{encode(value,Endian::little,Limits{5,1});},ErrorCode::resource_limit);
    expect_error([&]{decode_prefix(bytes,Endian::little,Limits{5,1});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello encode preflight accounts for all Package fields cumulatively") {
    CHECK(encode(fixtures[5].message,Endian::little,Limits{47,2}) == fixtures[5].little);
    expect_error([&]{encode(fixtures[5].message,Endian::little,Limits{46,2});},ErrorCode::resource_limit);
    expect_error([&]{decode_prefix(fixtures[5].little,Endian::little,Limits{46,2});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello scalar parser cannot read fields beyond its message budget") {
    const std::vector<std::uint8_t> bytes{4,0x78,0x56,0x34,0x12};
    expect_error([&]{decode_prefix(bytes,Endian::little,Limits{4,1});},ErrorCode::resource_limit);
    expect_error([&]{encode(NetSpeed{0x12345678},Endian::little,Limits{4,1});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello unit budget includes a transmitted terminator") {
    CHECK(encode(fixtures[1].message,Endian::little,Limits{12,3}) == fixtures[1].little);
    CHECK(decode_exact(fixtures[1].little,Endian::little,Limits{12,3}) == fixtures[1].message);
    expect_error([&]{encode(fixtures[1].message,Endian::little,Limits{12,2});},ErrorCode::resource_limit);
    expect_error([&]{decode_prefix(fixtures[1].little,Endian::little,Limits{12,2});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello long declared narrow count rejects before body allocation") {
    const std::vector<std::uint8_t> bytes{6,0xff,0xff,0xff,0x7f};
    expect_error([&]{decode_prefix(bytes,Endian::little,Limits{16,2});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello long declared wide count rejects before multiplication or allocation") {
    const std::vector<std::uint8_t> bytes{6,0x80,0,0,1}; // BE count -2147483647.
    expect_error([&]{decode_prefix(bytes,Endian::big,Limits{16,2});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello INT32_MIN prefix is invalid in both scalar endians") {
    const std::vector<std::uint8_t> little{6,0,0,0,0x80};
    const std::vector<std::uint8_t> big{6,0x80,0,0,0};
    expect_error([&]{decode_prefix(little,Endian::little,roomy);},ErrorCode::invalid_message);
    expect_error([&]{decode_prefix(big,Endian::big,roomy);},ErrorCode::invalid_message);
}
TEST_CASE("PostHello zero form cannot silently discard supplied units") {
    const Message value=Failure{{StringForm::zero,u"never-echo-private"}};
    expect_error([&]{encode(value,Endian::little,roomy);},ErrorCode::invalid_message);
}
TEST_CASE("PostHello narrow and wide forms require a nonzero count") {
    for (const auto form: {StringForm::narrow,StringForm::wide}) {
        expect_error([&]{encode(Failure{{form,{}}},Endian::little,roomy);},ErrorCode::invalid_message);
    }
}
TEST_CASE("PostHello unknown raw form is not accepted as wide") {
    const Message value=Failure{{static_cast<StringForm>(99),u"never-echo-private"}};
    expect_error([&]{encode(value,Endian::little,roomy);},ErrorCode::invalid_message);
}
TEST_CASE("PostHello narrow cannot truncate or replace a unit greater than ff") {
    const Message value=Failure{{StringForm::narrow,{0x0100}}};
    expect_error([&]{encode(value,Endian::little,roomy);},ErrorCode::invalid_message);
}
TEST_CASE("PostHello rejects every truncated literal including odd wide body") {
    for (const auto& f:fixtures) {
        for (const auto endian: {Endian::little,Endian::big}) {
            const auto& bytes=endian==Endian::little ? f.little : f.big;
            for (std::size_t length=0; length<bytes.size(); ++length) {
                CAPTURE(length);
                const auto input=std::span(bytes).first(length);
                expect_error([&]{decode_prefix(input,endian,roomy);},ErrorCode::truncated_archive);
                expect_error([&]{decode_exact(input,endian,roomy);},ErrorCode::truncated_archive);
            }
        }
    }
}
TEST_CASE("PostHello exact rejects any extra byte after each full literal") {
    for (const auto& f:fixtures) {
        auto bytes=f.little;
        bytes.push_back(0);
        expect_error([&]{decode_exact(bytes,Endian::little,roomy);},ErrorCode::extra_bytes);
    }
}
TEST_CASE("PostHello unsupported opcode does not use preHello or unknown formats") {
    for (const std::uint8_t opcode: {std::uint8_t{0},std::uint8_t{2},std::uint8_t{0x1a},std::uint8_t{0xff}}) {
        const std::vector<std::uint8_t> bytes{opcode};
        expect_error([&]{decode_prefix(bytes,Endian::little,roomy);},ErrorCode::unsupported_opcode);
        expect_error([&]{decode_exact(bytes,Endian::little,roomy);},ErrorCode::unsupported_opcode);
    }
}
TEST_CASE("PostHello error from Login budgets never includes Response or URL") {
    const Message value=Login{{StringForm::narrow,u"userinfo-guid-secret"},
        {StringForm::narrow,u"https://private.invalid"},{0,0,0,0}};
    expect_error([&]{encode(value,Endian::little,Limits{8,64});},ErrorCode::resource_limit);
}

namespace {
// Independent physical archives: signed counts/u32 use scalar endian, while
// wide bodies retain little-endian unit pairs. No codec builds these literals.
const std::array<Fixture,4> download_fixtures{{
    {DownloadMetadata{{StringForm::zero,{}},{StringForm::zero,{}},0x12345678},
        {0x0b,0,0,0,0,0,0,0,0,0x78,0x56,0x34,0x12},
        {0x0b,0,0,0,0,0,0,0,0,0x12,0x34,0x56,0x78}},
    {DownloadMetadata{{StringForm::narrow,{0x80,0xff,0}},
        {StringForm::wide,{0x0100,0xd800}},0xa0b0c0d0},
        {0x0b,3,0,0,0,0x80,0xff,0,0xfe,0xff,0xff,0xff,0,1,0,0xd8,0xd0,0xc0,0xb0,0xa0},
        {0x0b,0,0,0,3,0x80,0xff,0,0xff,0xff,0xff,0xfe,0,1,0,0xd8,0xa0,0xb0,0xc0,0xd0}},
    {DownloadMetadata{{StringForm::wide,{0x1234,0,0xdc00}},
        {StringForm::narrow,{0x58,0,0xff,0x5a}},0xffffffff},
        {0x0b,0xfd,0xff,0xff,0xff,0x34,0x12,0,0,0,0xdc,4,0,0,0,0x58,0,0xff,0x5a,0xff,0xff,0xff,0xff},
        {0x0b,0xff,0xff,0xff,0xfd,0x34,0x12,0,0,0,0xdc,0,0,0,4,0x58,0,0xff,0x5a,0xff,0xff,0xff,0xff}},
    {DownloadMetadata{{StringForm::wide,{0x0100,0}},
        {StringForm::narrow,{0x80,0xff,0}},0xfedcba98},
        {0x0b,0xfe,0xff,0xff,0xff,0,1,0,0,3,0,0,0,0x80,0xff,0,0x98,0xba,0xdc,0xfe},
        {0x0b,0xff,0xff,0xff,0xfe,0,1,0,0,0,0,0,3,0x80,0xff,0,0xfe,0xdc,0xba,0x98}}
}};

void check_download_fixture(std::size_t index) {
    const auto& f=download_fixtures[index];
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& bytes=endian==Endian::little ? f.little : f.big;
        CHECK(encode(f.message,endian,roomy)==bytes);
        CHECK(decode_exact(bytes,endian,roomy)==f.message);
        const auto decoded=decode_prefix(bytes,endian,roomy);
        CHECK(decoded.message==f.message);
        CHECK(decoded.consumed==bytes.size());
    }
}
}

TEST_CASE("PostHello DownloadMetadata zero strings and scalar literal") {
    check_download_fixture(0);
}
TEST_CASE("PostHello DownloadMetadata narrow ff and wide body remain distinct from scalar endian") {
    check_download_fixture(1);
}
TEST_CASE("PostHello DownloadMetadata preserves interior NUL and unpaired wide units") {
    check_download_fixture(2);
    check_download_fixture(3);
}
TEST_CASE("PostHello DownloadMetadata prefix budgets only its message before a coalesced NetSpeed") {
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& f=download_fixtures[0];
        auto bytes=endian==Endian::little ? f.little : f.big;
        const std::vector<std::uint8_t> suffix=endian==Endian::little
            ? std::vector<std::uint8_t>{4,0x78,0x56,0x34,0x12}
            : std::vector<std::uint8_t>{4,0x12,0x34,0x56,0x78};
        bytes.insert(bytes.end(),suffix.begin(),suffix.end());
        const auto first=decode_prefix(bytes,endian,Limits{13,1});
        CHECK(first.message==f.message);
        CHECK(first.consumed==13);
        const auto second=decode_prefix(std::span(bytes).subspan(first.consumed),endian,Limits{5,1});
        CHECK(second.message==Message{NetSpeed{0x12345678}});
        CHECK(second.consumed==5);
        expect_error([&]{decode_exact(bytes,endian,Limits{13,1});},ErrorCode::extra_bytes);
    }
}
TEST_CASE("PostHello DownloadMetadata prefix leaves unknown suffix unread but exact rejects it") {
    auto bytes=download_fixtures[0].little;
    bytes.push_back(0xff);
    const auto decoded=decode_prefix(bytes,Endian::little,Limits{13,1});
    CHECK(decoded.message==download_fixtures[0].message);
    CHECK(decoded.consumed==13);
    expect_error([&]{decode_exact(bytes,Endian::little,roomy);},ErrorCode::extra_bytes);
}
TEST_CASE("PostHello DownloadMetadata rejects every truncated string or final u32") {
    for (const auto& f:download_fixtures) {
        for (const auto endian: {Endian::little,Endian::big}) {
            const auto& bytes=endian==Endian::little ? f.little : f.big;
            for (std::size_t length=0; length<bytes.size(); ++length) {
                CAPTURE(length);
                const auto input=std::span(bytes).first(length);
                expect_error([&]{decode_prefix(input,endian,roomy);},ErrorCode::truncated_archive);
                expect_error([&]{decode_exact(input,endian,roomy);},ErrorCode::truncated_archive);
            }
        }
    }
}
TEST_CASE("PostHello DownloadMetadata byte budget includes both strings and complete scalar") {
    const auto& f=download_fixtures[1];
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& bytes=endian==Endian::little ? f.little : f.big;
        REQUIRE(bytes.size()==20);
        CHECK(encode(f.message,endian,Limits{20,3})==bytes);
        CHECK(decode_exact(bytes,endian,Limits{20,3})==f.message);
        expect_error([&]{encode(f.message,endian,Limits{19,3});},ErrorCode::resource_limit);
        expect_error([&]{decode_prefix(bytes,endian,Limits{19,3});},ErrorCode::resource_limit);
    }
}
TEST_CASE("PostHello DownloadMetadata enforces unit limits independently on both strings") {
    for (const std::size_t index: {std::size_t{1},std::size_t{3}}) {
        const auto& f=download_fixtures[index];
        for (const auto endian: {Endian::little,Endian::big}) {
            const auto& bytes=endian==Endian::little ? f.little : f.big;
            CHECK(encode(f.message,endian,Limits{20,3})==bytes);
            CHECK(decode_exact(bytes,endian,Limits{20,3})==f.message);
            expect_error([&]{encode(f.message,endian,Limits{20,2});},ErrorCode::resource_limit);
            expect_error([&]{decode_prefix(bytes,endian,Limits{20,2});},ErrorCode::resource_limit);
        }
    }
}
TEST_CASE("PostHello DownloadMetadata INT32_MIN is rejected in either string and endian") {
    const std::array<std::vector<std::uint8_t>,2> little{{
        {0x0b,0,0,0,0x80},{0x0b,0,0,0,0,0,0,0,0x80}}};
    const std::array<std::vector<std::uint8_t>,2> big{{
        {0x0b,0x80,0,0,0},{0x0b,0,0,0,0,0x80,0,0,0}}};
    for (const auto& bytes:little)
        expect_error([&]{decode_prefix(bytes,Endian::little,roomy);},ErrorCode::invalid_message);
    for (const auto& bytes:big)
        expect_error([&]{decode_prefix(bytes,Endian::big,roomy);},ErrorCode::invalid_message);
}
TEST_CASE("PostHello DownloadMetadata rejects oversized counts before either body is supplied") {
    const std::array<std::vector<std::uint8_t>,2> little{{
        {0x0b,3,0,0,0},{0x0b,0,0,0,0,3,0,0,0}}};
    const std::array<std::vector<std::uint8_t>,2> big{{
        {0x0b,0,0,0,3},{0x0b,0,0,0,0,0,0,0,3}}};
    for (const auto& bytes:little)
        expect_error([&]{decode_prefix(bytes,Endian::little,Limits{20,2});},ErrorCode::resource_limit);
    for (const auto& bytes:big)
        expect_error([&]{decode_prefix(bytes,Endian::big,Limits{20,2});},ErrorCode::resource_limit);
}
TEST_CASE("PostHello DownloadMetadata invalid forms on either string give fixed errors") {
    const std::array<RawString,5> invalid{{
        {StringForm::zero,u"never-echo-private"},
        {StringForm::narrow,{}},{StringForm::wide,{}},
        {StringForm::narrow,{0x0100}},
        {static_cast<StringForm>(99),u"never-echo-private"}
    }};
    for (const auto& text:invalid) {
        const Message first=DownloadMetadata{text,{StringForm::zero,{}},0};
        const Message second=DownloadMetadata{{StringForm::zero,{}},text,0};
        expect_error([&]{encode(first,Endian::little,roomy);},ErrorCode::invalid_message);
        expect_error([&]{encode(second,Endian::big,roomy);},ErrorCode::invalid_message);
    }
}


namespace {
// Compatibility: append PackageAck without renumbering the seven old variants.
static_assert(std::is_same_v<std::variant_alternative_t<0,Message>,Welcome>);
static_assert(std::is_same_v<std::variant_alternative_t<1,Message>,Challenge>);
static_assert(std::is_same_v<std::variant_alternative_t<2,Message>,NetSpeed>);
static_assert(std::is_same_v<std::variant_alternative_t<3,Message>,Login>);
static_assert(std::is_same_v<std::variant_alternative_t<4,Message>,Failure>);
static_assert(std::is_same_v<std::variant_alternative_t<5,Message>,Package>);
static_assert(std::is_same_v<std::variant_alternative_t<6,Message>,DownloadMetadata>);
static_assert(std::is_same_v<std::variant_alternative_t<7,Message>,PackageAck>);

// Independent physical archives from PackageAck8's five individually ordered
// u32 words. Neither identity nor generation is converted to a GUID/string.
// Wire proof: package-ack-wire/contract.json, producer4997d0/reader7867b0.
const Fixture package_ack{
    PackageAck{{0x11223344u,0x89abcdefu,0,0xffffffffu},0x80000001u},
    {8,0x44,0x33,0x22,0x11,0xef,0xcd,0xab,0x89,0,0,0,0,
       0xff,0xff,0xff,0xff,1,0,0,0x80},
    {8,0x11,0x22,0x33,0x44,0x89,0xab,0xcd,0xef,0,0,0,0,
       0xff,0xff,0xff,0xff,0x80,0,0,1}
};
}

TEST_CASE("PostHello PackageAck five unsigned words match literal LE and BE archives") {
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& bytes=endian==Endian::little ? package_ack.little : package_ack.big;
        CHECK(encode(package_ack.message,endian,Limits{21,1})==bytes);
        CHECK(decode_exact(bytes,endian,Limits{21,1})==package_ack.message);
        const auto decoded=decode_prefix(bytes,endian,Limits{21,1});
        CHECK(decoded.message==package_ack.message);
        CHECK(decoded.consumed==21);
    }
}
TEST_CASE("PostHello PackageAck accepts zero identity and zero generation without semantic policy") {
    const Message value=PackageAck{{0,0,0,0},0};
    const std::vector<std::uint8_t> bytes{8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    for (const auto endian: {Endian::little,Endian::big}) {
        CHECK(encode(value,endian,Limits{21,1})==bytes);
        CHECK(decode_exact(bytes,endian,Limits{21,1})==value);
    }
}
TEST_CASE("PostHello PackageAck rejects all incomplete lengths zero through twenty") {
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& bytes=endian==Endian::little ? package_ack.little : package_ack.big;
        for (std::size_t length=0; length<21; ++length) {
            CAPTURE(length);
            const auto input=std::span(bytes).first(length);
            expect_error([&]{decode_prefix(input,endian,Limits{21,1});},ErrorCode::truncated_archive);
            expect_error([&]{decode_exact(input,endian,Limits{21,1});},ErrorCode::truncated_archive);
        }
    }
}
TEST_CASE("PostHello PackageAck prefix consumes exactly twentyone before another message") {
    for (const auto endian: {Endian::little,Endian::big}) {
        auto bytes=endian==Endian::little ? package_ack.little : package_ack.big;
        const std::vector<std::uint8_t> suffix=endian==Endian::little
            ? std::vector<std::uint8_t>{4,0x78,0x56,0x34,0x12}
            : std::vector<std::uint8_t>{4,0x12,0x34,0x56,0x78};
        bytes.insert(bytes.end(),suffix.begin(),suffix.end());
        const auto first=decode_prefix(bytes,endian,Limits{21,1});
        CHECK(first.message==package_ack.message);
        CHECK(first.consumed==21);
        const auto second=decode_prefix(std::span(bytes).subspan(first.consumed),endian,Limits{5,1});
        CHECK(second.message==Message{NetSpeed{0x12345678}});
        CHECK(second.consumed==5);
        expect_error([&]{decode_exact(bytes,endian,Limits{21,1});},ErrorCode::extra_bytes);
    }
}
TEST_CASE("PostHello PackageAck exact rejects even an unknown singlebyte suffix") {
    for (const auto endian: {Endian::little,Endian::big}) {
        auto bytes=endian==Endian::little ? package_ack.little : package_ack.big;
        bytes.push_back(0xff);
        const auto first=decode_prefix(bytes,endian,Limits{21,1});
        CHECK(first.message==package_ack.message);
        CHECK(first.consumed==21);
        expect_error([&]{decode_exact(bytes,endian,roomy);},ErrorCode::extra_bytes);
    }
}
TEST_CASE("PostHello PackageAck requires twentyone archive bytes independently of string limit one") {
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto& bytes=endian==Endian::little ? package_ack.little : package_ack.big;
        CHECK(encode(package_ack.message,endian,Limits{21,1})==bytes);
        CHECK(decode_exact(bytes,endian,Limits{21,1})==package_ack.message);
        expect_error([&]{encode(package_ack.message,endian,Limits{20,1});},ErrorCode::resource_limit);
        expect_error([&]{decode_prefix(bytes,endian,Limits{20,1});},ErrorCode::resource_limit);
        expect_error([&]{decode_exact(bytes,endian,Limits{20,1});},ErrorCode::resource_limit);
    }
}

TEST_CASE("PostHello Join is appended without changing existing variant indices") {
    CHECK(std::variant_size_v<Message> == 9);
    CHECK(Message{Welcome{}}.index() == 0);
    CHECK(Message{PackageAck{}}.index() == 7);
    CHECK(Message{Join{}}.index() == 8);
}
TEST_CASE("PostHello Join matches independent singlebyte09 in both endians") {
    const std::vector<std::uint8_t> literal{0x09};
    for (const auto endian: {Endian::little,Endian::big}) {
        CHECK(encode(Join{},endian,Limits{1,1}) == literal);
        CHECK(decode_exact(literal,endian,Limits{1,1}) == Message{Join{}});
        const auto decoded=decode_prefix(literal,endian,Limits{1,1});
        CHECK(decoded.message == Message{Join{}});
        CHECK(decoded.consumed == 1);
    }
}
TEST_CASE("PostHello Join prefix ignores suffix beyond its onebyte budget") {
    const std::vector<std::uint8_t> literal{0x09,0x09,0xff};
    for (const auto endian: {Endian::little,Endian::big}) {
        const auto first=decode_prefix(literal,endian,Limits{1,1});
        CHECK(first.message == Message{Join{}});
        CHECK(first.consumed == 1);
        const auto second=decode_prefix(std::span(literal).subspan(first.consumed),endian,Limits{1,1});
        CHECK(second.message == Message{Join{}});
        CHECK(second.consumed == 1);
        expect_error([&]{decode_exact(literal,endian,Limits{1,1});},ErrorCode::extra_bytes);
    }
}
TEST_CASE("PostHello Join exact rejects any following byte") {
    for (const auto endian: {Endian::little,Endian::big}) {
        for (const std::uint8_t suffix: {std::uint8_t{0},std::uint8_t{9},std::uint8_t{0xff}}) {
            const std::vector<std::uint8_t> literal{0x09,suffix};
            expect_error([&]{decode_exact(literal,endian,roomy);},ErrorCode::extra_bytes);
        }
    }
}
TEST_CASE("PostHello Join cannot be decoded from an empty archive") {
    for (const auto endian: {Endian::little,Endian::big}) {
        expect_error([&]{decode_prefix({},endian,Limits{1,1});},ErrorCode::truncated_archive);
        expect_error([&]{decode_exact({},endian,Limits{1,1});},ErrorCode::truncated_archive);
    }
}
TEST_CASE("PostHello Join still requires valid limits and endian") {
    const std::vector<std::uint8_t> literal{0x09};
    for (const auto invalid: {Limits{0,1},Limits{1,0},
        Limits{1,static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1}}) {
        expect_error([&]{encode(Join{},Endian::little,invalid);},ErrorCode::invalid_limits);
        expect_error([&]{decode_exact(literal,Endian::little,invalid);},ErrorCode::invalid_limits);
    }
    const auto invalid=static_cast<Endian>(17);
    expect_error([&]{encode(Join{},invalid,Limits{1,1});},ErrorCode::invalid_endian);
    expect_error([&]{decode_prefix(literal,invalid,Limits{1,1});},ErrorCode::invalid_endian);
}
