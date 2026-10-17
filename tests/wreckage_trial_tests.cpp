#include "hawken/wreckage_trial.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>
#include <array>
#include <stdexcept>
#include <string>

namespace {
constexpr std::array<const char*,8> package_names{
    "Core.upk","Engine.upk","HawkenGame.upk","VS-Wreckage.upk","HawkenDM_SF.upk",
    "GameFramework.upk","UDKBase.upk","BaseAI.upk"};
const std::filesystem::path cooked="HawkenGame/CookedPCConsole";
void expect_asset_error(const std::filesystem::path& root) {
    try { (void)hawken::game::load_wreckage_trial(root); }
    catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "invalid Wreckage trial assets");
        return;
    }
    catch (const std::exception&) { FAIL_CHECK("expected fixed trial asset error"); return; }
    FAIL_CHECK("unverified trial assets accepted");
}
}

// Ordinary tests only touch private synthetic files, never require an installed
// client and cannot spoof a successful hash. Original-file success is checked
// separately by the parent-owned read-only probe, with exact plan assertions.
TEST_CASE("Wreckage trial rejects relative roots with a fixed error") {
    for (const std::filesystem::path root: {std::filesystem::path{},
             std::filesystem::path("."),std::filesystem::path("private-relative-root")})
        expect_asset_error(root);
}
TEST_CASE("Wreckage trial rejects an absent absolute root without creating it") {
    hawken::test::TempDirectory directory;
    const auto root=directory.path()/"absent-private-root";
    REQUIRE(root.is_absolute());
    expect_asset_error(root);
    CHECK_FALSE(std::filesystem::exists(root));
}
TEST_CASE("Wreckage trial rejects a regular file as game root without mutation") {
    hawken::test::TempDirectory directory;
    const auto root=directory.write("private-root-file","public synthetic root bytes");
    expect_asset_error(root);
    CHECK(hawken::test::read_bytes(root) == "public synthetic root bytes");
}
TEST_CASE("Wreckage trial rejects missing fixed packages in an existing directory") {
    hawken::test::TempDirectory directory;
    std::filesystem::create_directories(directory.path()/cooked);
    expect_asset_error(directory.path());
    for (const auto name:package_names)
        CHECK_FALSE(std::filesystem::exists(directory.path()/cooked/name));
}
TEST_CASE("Wreckage trial does not accept a partial dummy package set") {
    hawken::test::TempDirectory directory;
    const auto core=directory.write(cooked/"Core.upk","public synthetic package bytes");
    expect_asset_error(directory.path());
    CHECK(hawken::test::read_bytes(core) == "public synthetic package bytes");
    CHECK_FALSE(std::filesystem::exists(directory.path()/cooked/"Engine.upk"));
}
TEST_CASE("Wreckage trial rejects a complete altered dummy set without changing files") {
    hawken::test::TempDirectory directory;
    for (const auto name:package_names)
        directory.write(cooked/name,"public synthetic package bytes");
    const char* altered_name="HawkenDM_SF.upk";
    SUBCASE("Contained HGC container altered") {}
    SUBCASE("BaseAI prerequisite altered") { altered_name="BaseAI.upk"; }
    directory.write(cooked/altered_name,"public synthetic package bytes altered");
    expect_asset_error(directory.path());
    for (const auto name:package_names) {
        const auto expected=std::string(name)==altered_name
            ? "public synthetic package bytes altered" : "public synthetic package bytes";
        CHECK(hawken::test::read_bytes(directory.path()/cooked/name) == expected);
    }
}

TEST_CASE("Wreckage configuration absent root preserves UDP observation without links") {
    hawken::udp::UdpTrialConfig config{7777,"11111111-1111-4111-8111-111111111111","private-region"};
    hawken::game::configure_wreckage_trial(config,{});
    CHECK(config.port==7777);
    CHECK(config.server_guid=="11111111-1111-4111-8111-111111111111");
    CHECK(config.region=="private-region");
    CHECK_FALSE(config.publication);
    CHECK_FALSE(config.controller);
    CHECK_FALSE(config.pawn);
    CHECK_FALSE(config.pri);
    CHECK_FALSE(config.links);
}

TEST_CASE("Wreckage configuration failed asset gate preserves every existing trial choice") {
    // Public synthetic config markers, never native IDs or a hash-success fixture.
    hawken::udp::UdpTrialConfig config{12345,"22222222-2222-4222-8222-222222222222","marker-region"};
    config.publication=hawken::game::WorldPublication{};
    config.publication->packages.push_back({"PrivateMarker",{1,2,3,4},{5}});
    config.controller=hawken::udp::InitialControllerPlan{
        {7,41,42,true},{5,6,7,8},9,{10,11,12},hawken::wire::actor::Rotation{0,256,512},3};
    config.pawn=hawken::udp::InitialPawnPlan{
        {8,51,52,false},{9,10,11,12},13,{14,15,16},std::nullopt};
    config.pri=hawken::udp::InitialPriPlan{
        {9,61,62,false},{13,14,15,16},17,{18,19,20},hawken::wire::actor::Rotation{256,0,0}};
    config.links=hawken::udp::InitialActorLinksPlan{1,2,10,3,4,11};
    const auto before=config;
    hawken::test::TempDirectory directory;
    for (const auto name:package_names)
        directory.write(cooked/name,"public synthetic package bytes");
    CHECK_THROWS_WITH_AS(hawken::game::configure_wreckage_trial(config,directory.path()),
        "invalid Wreckage trial assets",std::runtime_error);
    CHECK(config.port==before.port);
    CHECK(config.server_guid==before.server_guid);
    CHECK(config.region==before.region);
    REQUIRE(config.publication);
    REQUIRE(config.publication->packages.size()==1);
    CHECK(config.publication->packages[0].name=="PrivateMarker");
    CHECK(config.publication->packages[0].identity==before.publication->packages[0].identity);
    CHECK(config.publication->packages[0].generation_net_counts==before.publication->packages[0].generation_net_counts);
    CHECK(config.publication->welcome==before.publication->welcome);
    CHECK(config.publication->contained_packages.empty());
    const auto unchanged_plan=[](const auto& actual,const auto& expected) {
        REQUIRE(actual);
        CHECK(actual->channel.channel==expected->channel.channel);
        CHECK(actual->channel.outgoing_sequence==expected->channel.outgoing_sequence);
        CHECK(actual->channel.incoming_sequence==expected->channel.incoming_sequence);
        CHECK(actual->channel.open_confirmed==expected->channel.open_confirmed);
        CHECK(actual->archetype_package_guid==expected->archetype_package_guid);
        CHECK(actual->archetype_local_net_index==expected->archetype_local_net_index);
        CHECK(actual->position==expected->position);
        CHECK(actual->rotation==expected->rotation);
    };
    unchanged_plan(config.controller,before.controller);
    unchanged_plan(config.pawn,before.pawn);
    unchanged_plan(config.pri,before.pri);
    CHECK(config.controller->net_player_index==before.controller->net_player_index);
    REQUIRE(config.links);
    CHECK(config.links->controller_pri_field_id==1);
    CHECK(config.links->controller_pawn_field_id==2);
    CHECK(config.links->controller_exclusive_bound==10);
    CHECK(config.links->pawn_controller_field_id==3);
    CHECK(config.links->pawn_pri_field_id==4);
    CHECK(config.links->pawn_exclusive_bound==11);
}

TEST_CASE("Wreckage configuration invalid nonempty root cannot enable actor links") {
    hawken::test::TempDirectory directory;
    auto root=directory.path()/"missing-private-root";
    SUBCASE("Missing absolute root") {}
    SUBCASE("Relative root") { root="private-relative-root"; }
    hawken::udp::UdpTrialConfig config{7777,"11111111-1111-4111-8111-111111111111","private-region"};
    CHECK_THROWS_WITH_AS(hawken::game::configure_wreckage_trial(config,root),
        "invalid Wreckage trial assets",std::runtime_error);
    CHECK(config.port==7777);
    CHECK(config.server_guid=="11111111-1111-4111-8111-111111111111");
    CHECK(config.region=="private-region");
    CHECK_FALSE(config.publication);
    CHECK_FALSE(config.controller);
    CHECK_FALSE(config.pawn);
    CHECK_FALSE(config.pri);
    CHECK_FALSE(config.links);
}

namespace {
hawken::udp::InventoryBootstrapPlan marker_inventory_plan() {
    const hawken::udp::InitialPawnPlan actor{
        {11,21,31,true},{1,2,3,4},5,{6,7,8},hawken::wire::actor::Rotation{0,256,512}};
    // Deliberately not a startup-valid full actor policy. Empty root and a
    // failed SHA gate must preserve it instead of validating or enabling it.
    return {actor,actor,actor,actor,25,80,150,15,14,21,21,23,24,31,23,35,29,32,34,
        {true,false,true,false,true,false,true,false}};
}
void check_marker_inventory_preserved(const hawken::udp::UdpTrialConfig& config) {
    REQUIRE(config.inventory_bootstrap);
    const auto& plan=*config.inventory_bootstrap;
    for (const auto* actor:{&plan.manager,&plan.dual_weapon,&plan.primary_weapon,&plan.secondary_weapon}) {
        CHECK(actor->channel.channel==11); CHECK(actor->channel.outgoing_sequence==21);
        CHECK(actor->channel.incoming_sequence==31); CHECK(actor->channel.open_confirmed);
        CHECK((actor->archetype_package_guid==std::array<std::uint32_t,4>{1,2,3,4}));
        CHECK(actor->archetype_local_net_index==5);
        CHECK((actor->position==hawken::wire::actor::QuantizedPosition{6,7,8}));
        CHECK((actor->rotation==std::optional{hawken::wire::actor::Rotation{0,256,512}}));
    }
    CHECK(plan.pawn_inv_manager_field_id==25); CHECK(plan.pawn_dual_weapon_field_id==80);
    CHECK(plan.pawn_exclusive_bound==150); CHECK(plan.manager_chain_field_id==21);
    CHECK(plan.manager_server_set_current_weapon_field_id==23); CHECK(plan.manager_exclusive_bound==24);
    CHECK(plan.dual_hand_weap_field_id==31); CHECK(plan.dual_client_given_to_field_id==23);
    CHECK(plan.dual_exclusive_bound==35); CHECK(plan.hand_dual_weapon_field_id==29);
    CHECK(plan.primary_exclusive_bound==32); CHECK(plan.secondary_exclusive_bound==34);
    CHECK(plan.eligibility.primary_package_map); CHECK_FALSE(plan.eligibility.actor_prefixes_correct);
    CHECK(plan.eligibility.archetypes_correct); CHECK_FALSE(plan.eligibility.actor_caches_correct);
    CHECK(plan.eligibility.scalar_links_correct); CHECK_FALSE(plan.eligibility.hand_weap_array_correct);
    CHECK(plan.eligibility.client_given_to_correct); CHECK_FALSE(plan.eligibility.server_set_current_weapon_correct);
}
}
TEST_CASE("Wreckage inventory policy absent root remains absent or preserves an explicit caller policy") {
    hawken::udp::UdpTrialConfig config{7777,"11111111-1111-4111-8111-111111111111","private-region"};
    hawken::game::configure_wreckage_trial(config,{});
    CHECK_FALSE(config.inventory_bootstrap);
    config.inventory_bootstrap=marker_inventory_plan();
    hawken::game::configure_wreckage_trial(config,{});
    check_marker_inventory_preserved(config);
    CHECK_FALSE(config.publication); CHECK_FALSE(config.controller); CHECK_FALSE(config.pawn);
    CHECK(config.port==7777); CHECK(config.region=="private-region");
}
TEST_CASE("Wreckage inventory policy failed original asset gate cannot replace an existing caller policy") {
    hawken::udp::UdpTrialConfig config{7777,"11111111-1111-4111-8111-111111111111","private-region"};
    config.inventory_bootstrap=marker_inventory_plan();
    hawken::test::TempDirectory directory;
    for (const auto name:package_names) directory.write(cooked/name,"public synthetic altered package");
    CHECK_THROWS_WITH_AS(hawken::game::configure_wreckage_trial(config,directory.path()),
        "invalid Wreckage trial assets",std::runtime_error);
    check_marker_inventory_preserved(config);
    CHECK_FALSE(config.publication); CHECK_FALSE(config.controller); CHECK_FALSE(config.pawn);
    CHECK(config.port==7777); CHECK(config.region=="private-region");
}
