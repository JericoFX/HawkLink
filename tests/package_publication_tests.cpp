#include "hawken/package_publication.hpp"
#include <doctest/doctest.h>
#include <limits>

namespace {
using namespace hawken::control;
using namespace hawken::control::posthello;
using namespace hawken::control::packages;
RootMetadata sample() {
    return {"A",{0x01020304,0x11223344,0,0x89abcdef},{7,2}};
}
constexpr Limits roomy{512,128};
void expect_error(const RootMetadata& metadata, Limits limits, ErrorCode code,
                  const char* message) {
    try { (void)make_root_package(metadata,limits); }
    catch (const Error& error) {
        CHECK(error.code() == code);
        CHECK(std::string(error.what()) == message);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed post-Hello Error"); return; }
    FAIL_CHECK("root publication accepted rejected metadata");
}
const std::vector<std::uint8_t> little_literal{
    7, 4,3,2,1, 0x44,0x33,0x22,0x11, 0,0,0,0, 0xef,0xcd,0xab,0x89,
    2,0,0,0, 0x41,0, // name0 A+NUL
    1,0,0,0, 0,      // name1 narrow NUL -> FName0
    0,0,0,0,         // text2 physical zero
    0,0,0,0, 2,0,0,0, // value0=0, generation COUNT2
    1,0,0,0, 0, 0};  // name3 narrow NUL, flags0
const std::vector<std::uint8_t> big_literal{
    7, 1,2,3,4, 0x11,0x22,0x33,0x44, 0,0,0,0, 0x89,0xab,0xcd,0xef,
    0,0,0,2, 0x41,0,
    0,0,0,1, 0,
    0,0,0,0,
    0,0,0,0, 0,0,0,2,
    0,0,0,1, 0, 0};
}

TEST_CASE("Root publication matches independent complete LE archive") {
    const auto result=make_root_package(sample(),roomy);
    CHECK(encode(result,Endian::little,roomy) == little_literal);
    CHECK(little_literal.size() == 46);
}
TEST_CASE("Root publication matches independent complete BE archive") {
    const auto result=make_root_package(sample(),roomy);
    CHECK(encode(result,Endian::big,roomy) == big_literal);
    CHECK(big_literal.size() == 46);
}
TEST_CASE("Root publication has explicit None fields and no download hints") {
    const auto result=make_root_package(sample(),roomy);
    const RawString none{StringForm::narrow,std::u16string(1,u'\0')};
    CHECK(result.name1 == none);
    CHECK(result.name3 == none);
    const RawString empty{StringForm::zero,{}};
    CHECK(result.text2 == empty);
    CHECK(result.value0 == 0);
    CHECK(result.flags == 0);
    const std::vector<std::uint8_t> expected{1,0,0,0,0};
    const auto bytes=encode(result,Endian::little,roomy);
    CHECK(std::vector<std::uint8_t>(bytes.begin()+23,bytes.begin()+28) == expected);
    CHECK(std::vector<std::uint8_t>(bytes.begin()+40,bytes.begin()+45) == expected);
}
TEST_CASE("Root publication preserves identity words including zero and high bits") {
    const auto metadata=sample();
    const auto result=make_root_package(metadata,roomy);
    CHECK(result.identity == metadata.identity);
}
TEST_CASE("Root publication value1 counts generations without interpreting netcounts") {
    auto metadata=sample();
    metadata.generation_net_counts={0xffffffff,0,7};
    const auto result=make_root_package(metadata,roomy);
    CHECK(result.value1 == 3);
    CHECK(result.value1 != metadata.generation_net_counts.back());
    CHECK(metadata.generation_net_counts == std::vector<std::uint32_t>{0xffffffff,0,7});
}
TEST_CASE("Root publication Engine metadata count differs from network and export counts") {
    const RootMetadata metadata{"Engine",{2471862799U,1177761918U,1116592536U,675424859U},{36697}};
    const auto result=make_root_package(metadata,roomy);
    CHECK(result.value1 == 1);
    CHECK(result.value1 != 36697);
    CHECK(result.value1 != 33928);
    CHECK(result.identity == metadata.identity);
    CHECK(encode(result,Endian::little,roomy).size() == 51);
}
TEST_CASE("Root publication does not require positive or monotonic netcounts") {
    auto metadata=sample();
    metadata.generation_net_counts={9,3,0,0};
    CHECK(make_root_package(metadata,roomy).value1 == 4);
    metadata.generation_net_counts={0};
    CHECK(make_root_package(metadata,roomy).value1 == 1);
}
TEST_CASE("Root publication accepts explicit ASCII root names and preserves case") {
    for (const std::string name: {"Root_09-A","aZ","123","None_1","NONE-1"}) {
        auto metadata=sample();metadata.name=name;
        const auto result=make_root_package(metadata,roomy);
        auto units=std::u16string(name.begin(),name.end());units.push_back(u'\0');
        const RawString expected{StringForm::narrow,units};
        CHECK(result.name0 == expected);
    }
}
TEST_CASE("Root publication permits partial-zero identity without creating a GUID") {
    auto metadata=sample();metadata.identity={0,0,0,1};
    CHECK(make_root_package(metadata,roomy).identity == metadata.identity);
}
TEST_CASE("Root publication preserves all caller metadata") {
    const auto metadata=sample();const auto before=metadata;
    (void)make_root_package(metadata,roomy);
    CHECK(metadata.name == before.name);
    CHECK(metadata.identity == before.identity);
    CHECK(metadata.generation_net_counts == before.generation_net_counts);
}
TEST_CASE("Root publication rejects empty name with fixed message") {
    auto metadata=sample();metadata.name.clear();
    expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Root publication rejects reserved None in every case") {
    for (const std::string name: {"None","none","NONE","nOnE"}) {
        auto metadata=sample();metadata.name=name;
        expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Root publication rejects paths and file extensions") {
    for (const std::string name: {"Root.upk","a/b","a\\b","C:Root",".",".."}) {
        auto metadata=sample();metadata.name=name;
        expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Root publication rejects spaces control characters and punctuation") {
    for (const std::string name: {"A B","A\nB","A\tB","A?B","A=B"}) {
        auto metadata=sample();metadata.name=name;
        expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Root publication rejects an embedded NUL without echoing it") {
    auto metadata=sample();metadata.name=std::string("A\0secret",8);
    expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Root publication rejects non-ASCII and DEL bytes") {
    for (const unsigned char byte: {static_cast<unsigned char>(0x7f),static_cast<unsigned char>(0x80),static_cast<unsigned char>(0xff)}) {
        auto metadata=sample();metadata.name.push_back(static_cast<char>(byte));
        expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Root publication rejects a null identity") {
    auto metadata=sample();metadata.identity={0,0,0,0};
    expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Root publication rejects missing generation metadata") {
    auto metadata=sample();metadata.generation_net_counts.clear();
    expect_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Root publication rejects zero archive budget") {
    expect_error(sample(),{0,128},ErrorCode::invalid_limits,"invalid post-Hello limits");
}
TEST_CASE("Root publication rejects zero string budget") {
    expect_error(sample(),{512,0},ErrorCode::invalid_limits,"invalid post-Hello limits");
}
TEST_CASE("Root publication rejects unrepresentable string budget") {
    const auto too_many=static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1;
    expect_error(sample(),{512,too_many},ErrorCode::invalid_limits,"invalid post-Hello limits");
}
TEST_CASE("Root publication honors exact string units including terminator") {
    const auto result=make_root_package(sample(),{46,2});
    CHECK(result.name0.units == std::u16string{u'A',u'\0'});
    expect_error(sample(),{46,1},ErrorCode::resource_limit,"post-Hello resource limit");
}
TEST_CASE("Root publication honors exact archive size and rejects size minus one") {
    CHECK(encode(make_root_package(sample(),{46,2}),Endian::little,{46,2}) == little_literal);
    expect_error(sample(),{45,2},ErrorCode::resource_limit,"post-Hello resource limit");
}
TEST_CASE("Root publication quotas are checked for the complete root name") {
    auto metadata=sample();metadata.name="Engine";
    CHECK(make_root_package(metadata,{51,7}).name0.units.size() == 7);
    expect_error(metadata,{50,7},ErrorCode::resource_limit,"post-Hello resource limit");
    expect_error(metadata,{51,6},ErrorCode::resource_limit,"post-Hello resource limit");
}
TEST_CASE("Root publication does not modify metadata when quota rejects it") {
    const auto metadata=sample();const auto before=metadata;
    expect_error(metadata,{45,2},ErrorCode::resource_limit,"post-Hello resource limit");
    CHECK(metadata.name == before.name);
    CHECK(metadata.identity == before.identity);
    CHECK(metadata.generation_net_counts == before.generation_net_counts);
}

namespace {
ContainedMetadata contained_sample() {
    return {{"HawkenGameContent",{0x5cd139c7,0x4aa73e14,0xbd1c8593,0xf86056dd},{533}},
        "HawkenDM_SF"};
}
void expect_contained_error(const ContainedMetadata& metadata, Limits limits,
                            ErrorCode code, const char* message) {
    try { (void)make_contained_package(metadata,limits); }
    catch (const Error& error) {
        CHECK(error.code() == code);
        CHECK(std::string(error.what()) == message);
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed post-Hello Error"); return; }
    FAIL_CHECK("contained publication accepted rejected metadata");
}
// Independent full archives: original HGC export180 GUID, generation COUNT1
// (not netcount533), selected no-download policy and explicit container name.
const std::vector<std::uint8_t> contained_little_literal{
    7, 0xc7,0x39,0xd1,0x5c, 0x14,0x3e,0xa7,0x4a,
    0x93,0x85,0x1c,0xbd, 0xdd,0x56,0x60,0xf8,
    18,0,0,0, 'H','a','w','k','e','n','G','a','m','e','C','o','n','t','e','n','t',0,
    1,0,0,0, 0, // name1 explicit None
    0,0,0,0,    // text2 physical zero
    0,0,0,0, 1,0,0,0, // value0 policy0; value1 generation COUNT1
    12,0,0,0, 'H','a','w','k','e','n','D','M','_','S','F',0, 0};
const std::vector<std::uint8_t> contained_big_literal{
    7, 0x5c,0xd1,0x39,0xc7, 0x4a,0xa7,0x3e,0x14,
    0xbd,0x1c,0x85,0x93, 0xf8,0x60,0x56,0xdd,
    0,0,0,18, 'H','a','w','k','e','n','G','a','m','e','C','o','n','t','e','n','t',0,
    0,0,0,1, 0,
    0,0,0,0,
    0,0,0,0, 0,0,0,1,
    0,0,0,12, 'H','a','w','k','e','n','D','M','_','S','F',0, 0};
}

TEST_CASE("Contained publication matches independent HGC LE archive") {
    const auto result=make_contained_package(contained_sample(),roomy);
    CHECK(encode(result,Endian::little,roomy) == contained_little_literal);
    CHECK(contained_little_literal.size() == 73);
}
TEST_CASE("Contained publication matches independent HGC BE archive") {
    const auto result=make_contained_package(contained_sample(),roomy);
    CHECK(encode(result,Endian::big,roomy) == contained_big_literal);
    CHECK(contained_big_literal.size() == 73);
}
TEST_CASE("Contained publication keeps None and local-only hints distinct from container") {
    const auto result=make_contained_package(contained_sample(),roomy);
    const RawString none{StringForm::narrow,std::u16string(1,u'\0')};
    const RawString empty{StringForm::zero,{}};
    const RawString container{StringForm::narrow,std::u16string(u"HawkenDM_SF",12)};
    CHECK(result.name1 == none);
    CHECK(result.text2 == empty);
    CHECK(result.name3 == container);
    CHECK(result.value0 == 0);
    CHECK(result.flags == 0);
}
TEST_CASE("Contained publication retains export identity and generation count not netcount") {
    const auto metadata=contained_sample();
    const auto result=make_contained_package(metadata,roomy);
    CHECK(result.identity == metadata.package.identity);
    CHECK(result.value1 == 1);
    CHECK(result.value1 != 533);
    CHECK(result.value1 != 1203);
    CHECK(result.identity != std::array<std::uint32_t,4>{0x1077e677,0x49227767,0xeb55cfbd,0x9ce404e4});
    auto changed=metadata;changed.package.generation_net_counts={533,0,2};
    CHECK(make_contained_package(changed,roomy).value1 == 3);
}
TEST_CASE("Contained publication preserves complete supplied metadata") {
    const auto metadata=contained_sample();const auto before=metadata;
    (void)make_contained_package(metadata,roomy);
    CHECK(metadata.package.name == before.package.name);
    CHECK(metadata.package.identity == before.package.identity);
    CHECK(metadata.package.generation_net_counts == before.package.generation_net_counts);
    CHECK(metadata.container_name == before.container_name);
}
TEST_CASE("Contained publication accepts distinct ASCII names and retains case") {
    auto metadata=contained_sample();metadata.container_name="Root_09-A";
    const auto result=make_contained_package(metadata,roomy);
    auto expected=std::u16string(u"Root_09-A");expected.push_back(u'\0');
    CHECK(result.name3 == RawString{StringForm::narrow,expected});
}
TEST_CASE("Contained publication rejects empty or reserved container name") {
    for (const std::string name: {"","None","none","NONE","nOnE"}) {
        auto metadata=contained_sample();metadata.container_name=name;
        expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Contained publication rejects paths extensions controls and non-ASCII container") {
    for (const std::string name: {"HawkenDM_SF.upk","a/b","a\\b","C:Root","A B","A\nB"}) {
        auto metadata=contained_sample();metadata.container_name=name;
        expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
    for (const unsigned char byte: {static_cast<unsigned char>(0x7f),static_cast<unsigned char>(0x80),static_cast<unsigned char>(0xff)}) {
        auto metadata=contained_sample();metadata.container_name.push_back(static_cast<char>(byte));
        expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
    auto metadata=contained_sample();metadata.container_name=std::string("A\0secret",8);
    expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Contained publication rejects self-container with ASCII folded comparison") {
    for (const std::string name: {"HawkenGameContent","hawkengamecontent","HAWKENGAMECONTENT"}) {
        auto metadata=contained_sample();metadata.container_name=name;
        expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    }
}
TEST_CASE("Contained publication retains root metadata validation") {
    auto metadata=contained_sample();metadata.package.name="None";
    expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    metadata=contained_sample();metadata.package.name="Package.upk";
    expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    metadata=contained_sample();metadata.package.identity={0,0,0,0};
    expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
    metadata=contained_sample();metadata.package.generation_net_counts.clear();
    expect_contained_error(metadata,roomy,ErrorCode::invalid_message,"invalid post-Hello message");
}
TEST_CASE("Contained publication rejects invalid explicit codec limits") {
    expect_contained_error(contained_sample(),{0,128},ErrorCode::invalid_limits,"invalid post-Hello limits");
    expect_contained_error(contained_sample(),{512,0},ErrorCode::invalid_limits,"invalid post-Hello limits");
    const auto too_many=static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())+1;
    expect_contained_error(contained_sample(),{512,too_many},ErrorCode::invalid_limits,"invalid post-Hello limits");
}
TEST_CASE("Contained publication budgets the combined names and terminators exactly") {
    const Limits exact{73,18};
    CHECK(encode(make_contained_package(contained_sample(),exact),Endian::little,exact) == contained_little_literal);
    expect_contained_error(contained_sample(),{72,18},ErrorCode::resource_limit,"post-Hello resource limit");
    expect_contained_error(contained_sample(),{73,17},ErrorCode::resource_limit,"post-Hello resource limit");
    auto metadata=contained_sample();metadata.package.name="A";metadata.container_name="LongContainer";
    CHECK(make_contained_package(metadata,{59,14}).name3.units.size() == 14);
    expect_contained_error(metadata,{58,14},ErrorCode::resource_limit,"post-Hello resource limit");
    expect_contained_error(metadata,{59,13},ErrorCode::resource_limit,"post-Hello resource limit");
}
TEST_CASE("Contained publication failed quota preserves both names identity and generations") {
    const auto metadata=contained_sample();const auto before=metadata;
    expect_contained_error(metadata,{72,18},ErrorCode::resource_limit,"post-Hello resource limit");
    CHECK(metadata.package.name == before.package.name);
    CHECK(metadata.package.identity == before.package.identity);
    CHECK(metadata.package.generation_net_counts == before.package.generation_net_counts);
    CHECK(metadata.container_name == before.container_name);
}
