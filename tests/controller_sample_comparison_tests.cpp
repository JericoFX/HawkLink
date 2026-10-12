#include "controller_sample_comparison.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <limits>

namespace {
using Json=nlohmann::json;
using hawken::probe::compare_controller_samples;
const std::array<std::uint64_t,6> trace={0x1000,0x48,7,0x2000,0x60,0};
Json metadata() {
    // Public synthetic metadata, not evidence of a live original process.
    return {{"mode","mech_setup"},{"name_count",100},{"field_ids_accepted",false},
        {"connection_cache",{{"status","not_requested"},{"archive_byte_swap_raw",0}}},
        {"structs",Json::array({{{"identity",{{"name","SMechSetupStruct"}}},
            {"members",Json::array({{{"identity",{{"name","AbilityIndex"}}},
                {"net_index",9},{"array_dim",1},{"enum_count",36}}})}}})}};
}
}

TEST_CASE("Equal mech metadata retains complete equality") {
    const auto first=metadata();
    const auto second=metadata();
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK(result.full_samples_equal);
    CHECK(result.inspected_metadata_equal);
    CHECK_FALSE(result.name_count_changed);
}

TEST_CASE("Only increasing global name count permits mech inspection equality") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=137;
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK_FALSE(result.full_samples_equal);
    CHECK(result.inspected_metadata_equal);
    CHECK(result.name_count_changed);
}

TEST_CASE("Positive signed name count boundaries remain representable") {
    auto first=metadata(); first["name_count"]=1;
    auto second=metadata(); second["name_count"]=std::numeric_limits<std::int32_t>::max();
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK(result.inspected_metadata_equal);
    CHECK_FALSE(result.full_samples_equal);
    CHECK(result.name_count_changed);
}

TEST_CASE("Declining name count does not pass mech comparison") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=99;
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK_FALSE(result.inspected_metadata_equal);
    CHECK_FALSE(result.full_samples_equal);
    CHECK(result.name_count_changed);
}

TEST_CASE("Missing global name count is rejected even when both samples match") {
    auto first=metadata(); first.erase("name_count");
    auto second=metadata();
    SUBCASE("Both absent") { second.erase("name_count"); }
    SUBCASE("First alone absent") {}
    SUBCASE("Second alone absent") { first=metadata(); second.erase("name_count"); }
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK_FALSE(result.inspected_metadata_equal);
}

TEST_CASE("Malformed or out of range name count cannot qualify as growth") {
    const Json bad_counts[]={0,-1,nullptr,"101",100.0,true,
        std::uint64_t{2147483648ULL},std::numeric_limits<std::uint64_t>::max()};
    for (const auto& bad:bad_counts) {
        auto first=metadata(); auto second=metadata();
        second["name_count"]=bad;
        CHECK_FALSE(compare_controller_samples(first,trace,second,trace,true).inspected_metadata_equal);
        first["name_count"]=bad; second["name_count"]=101;
        CHECK_FALSE(compare_controller_samples(first,trace,second,trace,true).inspected_metadata_equal);
        second["name_count"]=bad;
        CHECK_FALSE(compare_controller_samples(first,trace,second,trace,true).inspected_metadata_equal);
    }
}

TEST_CASE("Named member change remains a real mismatch during name growth") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=101;
    second["structs"][0]["members"][0]["identity"]["name"]="SetupID";
    const auto result=compare_controller_samples(first,trace,second,trace,true);
    CHECK_FALSE(result.inspected_metadata_equal);
    CHECK_FALSE(result.full_samples_equal);
}

TEST_CASE("Member index or enum count change cannot hide behind name growth") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=101;
    SUBCASE("NetIndex") { second["structs"][0]["members"][0]["net_index"]=10; }
    SUBCASE("Enum count") { second["structs"][0]["members"][0]["enum_count"]=37; }
    CHECK_FALSE(compare_controller_samples(first,trace,second,trace,true).inspected_metadata_equal);
}

TEST_CASE("Connection metadata or other root metadata must remain identical") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=101;
    SUBCASE("Connection archive order") { second["connection_cache"]["archive_byte_swap_raw"]=1; }
    SUBCASE("Additional root field") { second["extra"]=false; }
    SUBCASE("Removed root field") { second.erase("mode"); }
    CHECK_FALSE(compare_controller_samples(first,trace,second,trace,true).inspected_metadata_equal);
}

TEST_CASE("Equal metadata cannot conceal a changed typed pointer trace") {
    const auto first=metadata();
    auto second=metadata(); second["name_count"]=101;
    auto changed=trace; changed[0]=0x3000;
    const auto result=compare_controller_samples(first,trace,second,changed,true);
    CHECK_FALSE(result.inspected_metadata_equal);
    CHECK_FALSE(result.full_samples_equal);
}

TEST_CASE("Trace ordering and trace length remain strict") {
    const auto first=metadata(); const auto second=metadata();
    SUBCASE("Ordering") {
        auto changed=trace; changed[0]=trace[3]; changed[3]=trace[0];
        CHECK_FALSE(compare_controller_samples(first,trace,second,changed,true).inspected_metadata_equal);
    }
    SUBCASE("Length") {
        CHECK_FALSE(compare_controller_samples(first,trace,second,
            std::span<const std::uint64_t>(trace).first(5),true).inspected_metadata_equal);
    }
}

TEST_CASE("Non mech modes retain strict full sample comparison") {
    const auto first=metadata();
    auto second=metadata();
    SUBCASE("Exact equality") {
        const auto result=compare_controller_samples(first,trace,second,trace,false);
        CHECK(result.full_samples_equal);
        CHECK(result.inspected_metadata_equal);
        CHECK_FALSE(result.name_count_changed);
    }
    SUBCASE("Growth is not accepted") {
        second["name_count"]=101;
        const auto result=compare_controller_samples(first,trace,second,trace,false);
        CHECK_FALSE(result.full_samples_equal);
        CHECK_FALSE(result.inspected_metadata_equal);
        CHECK(result.name_count_changed);
    }
}
