#include "hawken/weapon_fire_state.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>

namespace {
namespace fire=hawken::game::weapon_fire;
fire::Scope bound_scope() { return {{1,2,3,4,5,6,7,8},7,11}; }
fire::Config recruit_config() { return {bound_scope(),{true,true,false,false}}; }
fire::Config four_hands() { return {bound_scope(),{true,true,true,true}}; }
fire::Authority authority() {
    // Explicit trial policy, not original CDO values or client-supplied facts.
    return {bound_scope(),true,true,true,
        {7,true,true,0,false,0.0F,false,false,false,0.0F,0.0F,1.5F,1.0F,false},false};
}
fire::Result start(std::uint8_t mode=0) { return {fire::Kind::start_fire,mode,std::nullopt,14}; }
fire::Result stop(std::uint8_t mode=0) { return {fire::Kind::stop_fire,mode,std::nullopt,14}; }
fire::Result extended(std::uint8_t mode,bool recent) { return {fire::Kind::stop_fire_extended,mode,recent,16}; }
void action(const fire::Step& value,std::size_t index,fire::ActionKind kind,std::uint8_t hand) {
    REQUIRE(value.action_count>index);
    CHECK(value.actions[index].kind==kind);
    CHECK(value.actions[index].hand==hand);
}
}

TEST_CASE("Weapon fire state consumes a real decoded start into pending intent and a begin action") {
    fire::FireState state(recruit_config());const auto context=authority();
    const std::array<std::uint8_t,2> literal{0x3a,0x00};
    hawken::wire::BitReader reader(literal,14);
    const fire::Schema schema{35,26,27,32,true,true,true,true};
    const auto decoded=fire::read_rpc(reader,schema,14);
    const auto result=state.apply(decoded,context,10.0);
    CHECK(result.accepted);CHECK(result.action_count==1);
    action(result,0,fire::ActionKind::begin_fire,0);
    CHECK((state.snapshot().pending_fire==std::array<bool,4>{true,false,false,false}));
    CHECK((state.snapshot().firing==std::array<bool,4>{true,false,false,false}));
    CHECK(state.snapshot().enabled);CHECK(reader.at_end());
}

TEST_CASE("Weapon fire state preserves native actions for repeated valid start and stop invocations") {
    fire::FireState state(recruit_config());const auto context=authority();
    for (unsigned repeat=0;repeat<2;++repeat) {
        const auto result=state.apply(start(),context,10.0);
        CHECK(result.accepted);CHECK(result.action_count==1);
        action(result,0,fire::ActionKind::begin_fire,0);
    }
    CHECK(state.apply(start(1),context,10.0).accepted);
    for (unsigned repeat=0;repeat<2;++repeat) {
        const auto result=state.apply(stop(),context,10.0);
        CHECK(result.accepted);CHECK(result.action_count==1);
        action(result,0,fire::ActionKind::end_fire,0);
    }
    CHECK((state.snapshot().pending_fire==std::array<bool,4>{false,true,false,false}));
    CHECK((state.snapshot().firing==std::array<bool,4>{false,true,false,false}));
}

TEST_CASE("Weapon fire state keeps blocked intent and resumes pending hands on permission recovery") {
    fire::FireState state(recruit_config());auto context=authority();context.pawn.overheated=true;
    const auto blocked=state.apply(start(),context,1.0);
    CHECK(blocked.accepted);CHECK(blocked.action_count==0);
    CHECK(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().firing[0]);
    CHECK_FALSE(state.snapshot().enabled);
    context.pawn.overheated=false;
    const auto resumed=state.tick(context,1.1);
    CHECK(resumed.accepted);CHECK(resumed.action_count==1);
    action(resumed,0,fire::ActionKind::begin_fire,0);
    CHECK(state.snapshot().firing[0]);CHECK(state.snapshot().enabled);
    context.client_shutdown=true;
    const auto shutdown=state.tick(context,1.2);
    CHECK(shutdown.accepted);CHECK(shutdown.action_count==2);
    action(shutdown,0,fire::ActionKind::end_fire,0);action(shutdown,1,fire::ActionKind::end_fire,1);
    CHECK(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().firing[0]);
    context.client_shutdown=false;
    const auto restarted=state.tick(context,1.3);
    CHECK(restarted.action_count==1);action(restarted,0,fire::ActionKind::begin_fire,0);
}

TEST_CASE("Weapon fire state stop clears pending even after combat permission is lost") {
    fire::FireState state(recruit_config());auto context=authority();
    CHECK(state.apply(start(),context,10.0).accepted);
    context.pawn.weapons_down=true;
    const auto result=state.apply(stop(),context,10.1);
    CHECK(result.accepted);CHECK(result.action_count==3);
    action(result,0,fire::ActionKind::end_fire,0);action(result,1,fire::ActionKind::end_fire,1);
    action(result,2,fire::ActionKind::end_fire,0);
    CHECK_FALSE(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().firing[0]);
    CHECK_FALSE(state.snapshot().enabled);
}

TEST_CASE("Weapon fire state recent extended stop waits exactly the original float timer delay") {
    fire::FireState state(recruit_config());const auto context=authority();
    CHECK(state.apply(start(),context,10.0).accepted);
    const auto requested=state.apply(extended(0,true),context,10.1);
    CHECK(requested.accepted);CHECK(requested.action_count==0);
    CHECK(state.snapshot().pending_fire[0]);CHECK(state.snapshot().firing[0]);
    const double deadline=10.1+static_cast<double>(0.2F);
    CHECK(state.snapshot().stop_deadlines[0]==std::optional<double>{deadline});
    const auto before=state.tick(context,deadline-0.000001);
    CHECK(before.accepted);CHECK(before.action_count==0);CHECK(state.snapshot().pending_fire[0]);
    const auto at=state.tick(context,deadline);
    CHECK(at.accepted);CHECK(at.action_count==1);action(at,0,fire::ActionKind::end_fire,0);
    CHECK_FALSE(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().firing[0]);
    CHECK_FALSE(state.snapshot().stop_deadlines[0]);
    const auto repeated=state.tick(context,deadline);
    CHECK(repeated.accepted);CHECK(repeated.action_count==0);
}

TEST_CASE("Weapon fire state extended false and hands two and three end immediately") {
    fire::FireState state(four_hands());const auto context=authority();
    for (std::uint8_t hand=0;hand<4;++hand) {
        const double begin_time=1.0+2.0*hand;
        CHECK(state.apply(start(hand),context,begin_time).accepted);
        const auto result=state.apply(extended(hand,hand>=2),context,begin_time+0.1);
        CHECK(result.accepted);CHECK(result.action_count==1);
        action(result,0,fire::ActionKind::end_fire,hand);
        CHECK_FALSE(state.snapshot().pending_fire[hand]);CHECK_FALSE(state.snapshot().firing[hand]);
    }
    CHECK_FALSE(state.snapshot().stop_deadlines[0]);CHECK_FALSE(state.snapshot().stop_deadlines[1]);
}

TEST_CASE("Weapon fire state start cancels only its own delayed stop") {
    fire::FireState state(recruit_config());const auto context=authority();
    CHECK(state.apply(start(),context,1.0).accepted);CHECK(state.apply(start(1),context,1.0).accepted);
    CHECK(state.apply(extended(0,true),context,2.0).accepted);
    CHECK(state.apply(extended(1,true),context,2.0).accepted);
    const auto renewed=state.apply(start(),context,2.1);
    CHECK(renewed.accepted);CHECK(renewed.action_count==1);
    CHECK_FALSE(state.snapshot().stop_deadlines[0]);
    CHECK(state.snapshot().stop_deadlines[1]==std::optional<double>{2.0+static_cast<double>(0.2F)});
    const auto due=state.tick(context,2.0+static_cast<double>(0.2F));
    CHECK(due.action_count==1);action(due,0,fire::ActionKind::end_fire,1);
    CHECK(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().pending_fire[1]);
}

TEST_CASE("Weapon fire state repeated recent stops rearm the same one-shot timer") {
    fire::FireState state(recruit_config());const auto context=authority();
    CHECK(state.apply(start(),context,1.0).accepted);
    CHECK(state.apply(extended(0,true),context,2.0).accepted);
    CHECK(state.apply(extended(0,true),context,2.1).accepted);
    const auto old_deadline=state.tick(context,2.0+static_cast<double>(0.2F));
    CHECK(old_deadline.accepted);CHECK(old_deadline.action_count==0);CHECK(state.snapshot().pending_fire[0]);
    const auto renewed=state.tick(context,2.1+static_cast<double>(0.2F));
    CHECK(renewed.action_count==1);action(renewed,0,fire::ActionKind::end_fire,0);
}

TEST_CASE("Weapon fire state has no hidden timer expiry during RPC application") {
    fire::FireState state(recruit_config());const auto context=authority();
    CHECK(state.apply(start(),context,1.0).accepted);
    CHECK(state.apply(extended(0,true),context,2.0).accepted);
    const auto other_hand=state.apply(start(1),context,3.0);
    CHECK(other_hand.accepted);CHECK(other_hand.action_count==1);
    action(other_hand,0,fire::ActionKind::begin_fire,1);
    CHECK(state.snapshot().pending_fire[0]);
    const auto due=state.tick(context,3.0);
    CHECK(due.action_count==1);action(due,0,fire::ActionKind::end_fire,0);
    CHECK(state.snapshot().pending_fire[1]);
}

TEST_CASE("Weapon fire state retains all four-hand restart and explicit start actions") {
    fire::FireState state(four_hands());auto context=authority();context.pawn.overheated=true;
    for (std::uint8_t hand=0;hand<4;++hand) CHECK(state.apply(start(hand),context,1.0).accepted);
    context.pawn.overheated=false;
    const auto restarted=state.apply(start(),context,1.1);
    CHECK(restarted.accepted);CHECK(restarted.action_count==5);
    for (std::uint8_t hand=0;hand<4;++hand) action(restarted,hand,fire::ActionKind::begin_fire,hand);
    action(restarted,4,fire::ActionKind::begin_fire,0);
    CHECK((state.snapshot().firing==std::array<bool,4>{true,true,true,true}));
}

TEST_CASE("Weapon fire state fixed action buffer preserves shutdown plus both expired timer calls") {
    fire::FireState state(four_hands());auto context=authority();
    for (std::uint8_t hand=0;hand<4;++hand) CHECK(state.apply(start(hand),context,1.0).accepted);
    CHECK(state.apply(extended(0,true),context,2.0).accepted);
    CHECK(state.apply(extended(1,true),context,2.0).accepted);
    context.pawn.overheated=true;
    const auto result=state.tick(context,2.0+static_cast<double>(0.2F));
    CHECK(result.accepted);CHECK(result.action_count==6);
    for (std::uint8_t hand=0;hand<4;++hand) action(result,hand,fire::ActionKind::end_fire,hand);
    action(result,4,fire::ActionKind::end_fire,0);action(result,5,fire::ActionKind::end_fire,1);
    CHECK((state.snapshot().pending_fire==std::array<bool,4>{false,false,true,true}));
    CHECK((state.snapshot().firing==std::array<bool,4>{false,false,false,false}));
}

TEST_CASE("Weapon fire state rejects stale foreign unauthenticated and unowned authority without mutation") {
    for (unsigned invalid=0;invalid<7;++invalid) {
        CAPTURE(invalid);fire::FireState state(recruit_config());auto context=authority();
        CHECK(state.apply(start(),context,10.0).accepted);const auto before=state.snapshot();
        if (invalid==0) context.scope.session_id[0]=99;
        if (invalid==1) context.scope.spawn_revision=8;
        if (invalid==2) context.scope.actor_channel=12;
        if (invalid==3) context.authenticated=false;
        if (invalid==4) context.actor_owned=false;
        if (invalid==5) context.instigator_valid=false;
        if (invalid==6) context.pawn.spawn_revision=8;
        const auto result=state.apply(stop(),context,10.1);
        CHECK_FALSE(result.accepted);CHECK(result.action_count==0);CHECK(state.snapshot()==before);
        const auto idle=state.tick(context,10.1);
        CHECK_FALSE(idle.accepted);CHECK(idle.action_count==0);CHECK(state.snapshot()==before);
    }
}

TEST_CASE("Weapon fire state dead or inactive start is rejected but an owned stop can clear intent") {
    for (const bool inactive:{false,true}) {
        CAPTURE(inactive);fire::FireState state(recruit_config());auto context=authority();
        CHECK(state.apply(start(),context,1.0).accepted);
        if (inactive) context.pawn.spawn_active=false; else context.pawn.alive=false;
        const auto before=state.snapshot();
        CHECK_FALSE(state.apply(start(1),context,2.0).accepted);CHECK(state.snapshot()==before);
        const auto stopped=state.apply(stop(),context,2.0);
        CHECK(stopped.accepted);CHECK(stopped.action_count==3);
        CHECK_FALSE(state.snapshot().pending_fire[0]);CHECK_FALSE(state.snapshot().firing[0]);
    }
}

TEST_CASE("Weapon fire state rejects unsupported hands and inconsistent typed RPC records") {
    for (unsigned invalid=0;invalid<7;++invalid) {
        CAPTURE(invalid);fire::FireState state(recruit_config());const auto context=authority();
        CHECK(state.apply(start(),context,1.0).accepted);const auto before=state.snapshot();
        auto request=start(2);
        if (invalid==1) request=start(3);
        if (invalid==2) request=start(4);
        if (invalid==3) request=start(255);
        if (invalid==4) {request=start();request.kind=static_cast<fire::Kind>(255);}
        if (invalid==5) {request=start();request.recent_fire=true;}
        if (invalid==6) {request=extended(0,true);request.recent_fire.reset();}
        const auto result=state.apply(request,context,2.0);
        CHECK_FALSE(result.accepted);CHECK(result.action_count==0);CHECK(state.snapshot()==before);
    }
}

TEST_CASE("Weapon fire state invalid time does not commit a clock state or pending change") {
    const std::array invalid_times{-1.0,9.0,std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()};
    for (const auto time:invalid_times) {
        fire::FireState state(recruit_config());const auto context=authority();
        CHECK(state.apply(start(),context,10.0).accepted);const auto before=state.snapshot();
        CHECK_FALSE(state.apply(stop(),context,time).accepted);CHECK(state.snapshot()==before);
        CHECK_FALSE(state.tick(context,time).accepted);CHECK(state.snapshot()==before);
        CHECK(state.apply(stop(),context,10.0).accepted);
    }
}

TEST_CASE("Weapon fire state copies immutable binding and separates replacement spawns") {
    auto config=recruit_config();fire::FireState old_spawn(config);
    config.scope.session_id[0]=99;config.supported_hands[0]=false;
    auto context=authority();CHECK(old_spawn.apply(start(),context,0.0).accepted);
    CHECK(old_spawn.snapshot().pending_fire[0]);
    auto replacement_config=recruit_config();replacement_config.scope.spawn_revision=8;
    fire::FireState replacement(replacement_config);
    context.scope.spawn_revision=8;context.pawn.spawn_revision=8;
    CHECK_FALSE(old_spawn.apply(stop(),context,1.0).accepted);
    const auto result=replacement.apply(start(1),context,0.0);
    CHECK(result.accepted);CHECK(result.action_count==1);action(result,0,fire::ActionKind::begin_fire,1);
    CHECK((replacement.snapshot().pending_fire==std::array<bool,4>{false,true,false,false}));
}

TEST_CASE("Weapon fire state invalid configured scope never grants intent or firing") {
    for (unsigned invalid=0;invalid<5;++invalid) {
        CAPTURE(invalid);auto config=recruit_config();
        if (invalid==0) config.scope.session_id.fill(0);
        if (invalid==1) config.scope.spawn_revision=0;
        if (invalid==2) config.scope.actor_channel=0;
        if (invalid==3) config.scope.actor_channel=2048;
        if (invalid==4) config.supported_hands.fill(false);
        fire::FireState state(config);auto context=authority();context.scope=config.scope;
        context.pawn.spawn_revision=config.scope.spawn_revision;
        const auto before=state.snapshot();
        CHECK_FALSE(state.apply(start(),context,0.0).accepted);CHECK(state.snapshot()==before);
        CHECK_FALSE(state.tick(context,0.0).accepted);CHECK(state.snapshot()==before);
    }
}
