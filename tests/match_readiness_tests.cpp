#include "hawken/match_readiness.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace {
using namespace hawken::game;
PendingMatchRules rules() { return {true,1,0.55F,5,0,10,30.0F}; }
PendingMatchState initial() { return {0.0F,0.0F,false,9,7,-1}; }
const std::array<MatchPlayerState,1> ready_player{{{true,true,true,true,false,false}}};
PendingMatchInputs inputs(float now, std::span<const MatchPlayerState> roster=ready_player) {
    return {now,1,0,roster,true,false,true,false};
}
template<class Operation>
void invalid(Operation operation) {
    CHECK_THROWS_WITH_AS(operation(),"Invalid match readiness.",std::invalid_argument);
}
}

TEST_CASE("PendingMatch publishes preminimum countdown before activating its timer") {
    PendingMatch model(rules(),initial());
    CHECK_FALSE(model.evaluate(inputs(0.0F)).start_requested);
    CHECK(model.snapshot().waiting_for == 0);
    CHECK(model.snapshot().waiting_to_ready == 0);
    CHECK(model.snapshot().countdown_remaining == 15);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK_FALSE(model.evaluate(inputs(4.75F)).start_requested);
    CHECK(model.snapshot().countdown_remaining == 10); // trunc(10+5-4.75)
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK_FALSE(model.evaluate(inputs(5.0F)).start_requested);
    CHECK(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_begin_time == 5.0F);
    CHECK(model.snapshot().countdown_remaining == 10);
}

TEST_CASE("PendingMatch countdown uses begin time and start action is unique without invented countdown zero") {
    PendingMatchState seed{0.0F,5.0F,true,0,0,7};
    PendingMatch model(rules(),seed);
    REQUIRE(model.evaluate(inputs(15.0F)).start_requested);
    CHECK(model.snapshot().countdown_remaining == 7);
    CHECK_FALSE(model.evaluate(inputs(16.0F)).start_requested);
    CHECK_FALSE(model.evaluate(inputs(17.0F)).start_requested);
    CHECK(model.snapshot().countdown_remaining == 7);
    PendingMatch fresh(rules(),seed);
    CHECK_FALSE(fresh.evaluate(inputs(14.9F)).start_requested);
    CHECK(fresh.snapshot().countdown_remaining == 0);
    CHECK(fresh.evaluate(inputs(15.0F)).start_requested);
}

TEST_CASE("PendingMatch historical begun flag bypasses readiness but is not the emitted action latch") {
    auto config=rules();config.min_players=5;config.min_percent_ready=1.0F;
    PendingMatch model(config,initial());
    const std::array<MatchPlayerState,1> unready{{{true,true,false,true,false,false}}};
    auto event=inputs(5.0F,unready);event.match_has_begun=true;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().waiting_for == 0);
    CHECK(model.snapshot().waiting_to_ready == 0);
    CHECK(model.snapshot().timer_active);
    event.now=15.0F;
    CHECK(model.evaluate(event).start_requested);
    CHECK_FALSE(model.evaluate(event).start_requested);
}

TEST_CASE("PendingMatch population shortage returns before readiness and preserves old waitingReady") {
    auto config=rules();config.min_players=5;config.min_percent_ready=1.0F;
    PendingMatch model(config,initial());
    auto event=inputs(5.0F);event.num_players=2;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().waiting_for == 3);
    CHECK(model.snapshot().waiting_to_ready == 7);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_remaining == -1);
}

TEST_CASE("PendingMatch zero total needs one and preserves both counters even with zero minimum") {
    auto config=rules();config.min_players=0;config.min_percent_ready=0.0F;
    PendingMatch model(config,initial());
    auto event=inputs(5.0F,{});event.num_players=0;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot() == initial());
}

TEST_CASE("PendingMatch bypass rules clear both counters without requiring ready roster") {
    const std::array<MatchPlayerState,1> unready{{{true,true,false,true,false,false}}};
    for (unsigned mode=0;mode<3;++mode) {
        CAPTURE(mode);
        auto config=rules();config.min_players=7;config.min_percent_ready=1.0F;
        if (mode==0) config.players_must_be_ready=false;
        if (mode==1) config.min_players=0;
        if (mode==2) config.min_percent_ready=0.0F;
        PendingMatch model(config,initial());
        CHECK_FALSE(model.evaluate(inputs(5.0F,unready)).start_requested);
        CHECK(model.snapshot().waiting_for == 0);
        CHECK(model.snapshot().waiting_to_ready == 0);
        CHECK(model.snapshot().timer_active);
    }
    auto config=rules();config.players_must_be_ready=false;
    PendingMatch empty(config,initial());
    auto event=inputs(5.0F,{});event.num_players=0;
    CHECK_FALSE(empty.evaluate(event).start_requested);
    CHECK(empty.snapshot() == initial()); // CheckPlayers early false before bypass.
}

TEST_CASE("PendingMatch ratio uses connected NumPlayers while population includes travelling") {
    auto config=rules();config.min_players=4;config.min_percent_ready=0.55F;
    PendingMatch model(config,initial());
    auto event=inputs(31.0F);event.num_players=2;event.num_travelling_players=2;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().waiting_for == 0); // population4 satisfies min4.
    CHECK(model.snapshot().waiting_to_ready == 1); // ceil(2*.55)-oneReady=1, not2.
    CHECK_FALSE(model.snapshot().timer_active);
}

TEST_CASE("PendingMatch counts only ready waiting human players with a concrete caller supplied controller roster") {
    const std::array<MatchPlayerState,4> roster{{
        {false,true,true,true,false,false},
        {true,false,true,true,false,false},
        {true,true,false,true,false,false},
        {true,true,true,true,false,false}
    }};
    auto config=rules();config.min_percent_ready=1.0F;
    PendingMatch model(config,initial());
    auto event=inputs(5.0F,roster);event.num_players=4;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().waiting_to_ready == 3);
    CHECK_FALSE(model.snapshot().timer_active);
}

TEST_CASE("PendingMatch becoming unready cancels timer and becoming ready rearms from current time") {
    PendingMatch model(rules(),initial());
    CHECK_FALSE(model.evaluate(inputs(5.0F)).start_requested);
    const std::array<MatchPlayerState,1> unready{{{true,true,false,true,false,false}}};
    CHECK_FALSE(model.evaluate(inputs(6.0F,unready)).start_requested);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_begin_time == 100000000000.0F);
    CHECK(model.snapshot().countdown_remaining == -1);
    CHECK(model.snapshot().waiting_to_ready == 1);
    CHECK_FALSE(model.evaluate(inputs(9.0F)).start_requested);
    CHECK(model.snapshot().countdown_begin_time == 9.0F);
    CHECK(model.snapshot().countdown_remaining == 10);
    CHECK_FALSE(model.evaluate(inputs(18.9F)).start_requested);
    CHECK(model.evaluate(inputs(19.0F)).start_requested);
}

TEST_CASE("PendingMatch deathmatch sync purchasing and saving gates preserve active timers and counters") {
    const PendingMatchState seed{0.0F,5.0F,true,8,6,4};
    for (unsigned gate=0;gate<3;++gate) {
        CAPTURE(gate);
        PendingMatch model(rules(),seed);
        auto roster=ready_player;
        if (gate==0) roster[0].online_synced=false;
        if (gate==1) roster[0].purchasing=true;
        if (gate==2) roster[0].saving=true;
        CHECK_FALSE(model.evaluate(inputs(100.0F,roster)).start_requested);
        CHECK(model.snapshot() == seed);
        CHECK(model.evaluate(inputs(101.0F)).start_requested);
    }
}

TEST_CASE("PendingMatch tracking and pending quit gates preserve timer and counters") {
    const PendingMatchState seed{0.0F,5.0F,true,8,6,4};
    for (unsigned gate=0;gate<2;++gate) {
        PendingMatch model(rules(),seed);
        auto event=inputs(100.0F);
        if (gate==0) event.tracking_created=false;
        if (gate==1) event.pending_quit=true;
        CHECK_FALSE(model.evaluate(event).start_requested);
        CHECK(model.snapshot() == seed);
        CHECK(model.evaluate(inputs(101.0F)).start_requested);
    }
}

TEST_CASE("PendingMatch travelling blocks exactly thirty seconds and cancels an active timer") {
    const PendingMatchState seed{0.0F,5.0F,true,8,6,4};
    PendingMatch model(rules(),seed);
    auto event=inputs(30.0F);event.num_travelling_players=1;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_remaining == -1);
    CHECK(model.snapshot().waiting_for == 0);
    CHECK(model.snapshot().waiting_to_ready == 0);
    event.now=30.01F;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_begin_time == 30.01F);
    CHECK(model.snapshot().countdown_remaining == 10);
}

TEST_CASE("PendingMatch maximum delay truncates elapsed int and does not replace minimum delay") {
    auto config=rules();config.min_delay_seconds=100;config.max_delay_seconds=4;
    PendingMatch model(config,initial());
    const std::array<MatchPlayerState,1> unready{{{true,true,false,true,false,false}}};
    CHECK_FALSE(model.evaluate(inputs(3.99F,unready)).start_requested);
    CHECK(model.snapshot().waiting_to_ready == 1);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK_FALSE(model.evaluate(inputs(4.0F,unready)).start_requested);
    CHECK(model.snapshot().waiting_to_ready == 0);
    CHECK(model.snapshot().waiting_for == 0);
    CHECK(model.snapshot().countdown_remaining == 106);
    CHECK_FALSE(model.snapshot().timer_active);
}

TEST_CASE("PendingMatch not waiting bypasses countdown only after minimum eligible delay") {
    PendingMatch model(rules(),initial());
    auto event=inputs(4.0F);event.waiting_to_start_match=false;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot().countdown_remaining == 11);
    event.now=5.0F;
    CHECK(model.evaluate(event).start_requested);
    CHECK_FALSE(model.snapshot().timer_active);
    CHECK(model.snapshot().countdown_remaining == 11);
    CHECK_FALSE(model.evaluate(event).start_requested);
}

TEST_CASE("PendingMatch owns copied world rules and seed independent from caller mutation") {
    auto config=rules();auto seed=initial();
    PendingMatch model(config,seed);
    config.min_delay_seconds=100;config.countdown_seconds=99;seed.waiting_for=77;
    CHECK_FALSE(model.evaluate(inputs(5.0F)).start_requested);
    CHECK(model.snapshot().countdown_begin_time == 5.0F);
    CHECK(model.snapshot().countdown_remaining == 10);
    CHECK(model.evaluate(inputs(15.0F)).start_requested);
}

TEST_CASE("PendingMatch rejects invalid numeric rules and seeds without a native behavior claim") {
    for (unsigned bad=0;bad<10;++bad) {
        CAPTURE(bad);
        auto config=rules();
        if (bad==0) config.min_players=-1;
        if (bad==1) config.min_percent_ready=-0.1F;
        if (bad==2) config.min_percent_ready=1.1F;
        if (bad==3) config.min_percent_ready=std::numeric_limits<float>::quiet_NaN();
        if (bad==4) config.min_delay_seconds=-1;
        if (bad==5) config.max_delay_seconds=-1;
        if (bad==6) config.countdown_seconds=-1;
        if (bad==7) config.max_wait_for_travelling_seconds=-1.0F;
        if (bad==8) config.max_wait_for_travelling_seconds=std::numeric_limits<float>::infinity();
        if (bad==9) config.min_percent_ready=std::numeric_limits<float>::infinity();
        invalid([&] { PendingMatch rejected(config,initial()); });
    }
    for (unsigned bad=0;bad<6;++bad) {
        auto seed=initial();
        if (bad==0) seed.pending_start_time=-1.0F;
        if (bad==1) seed.pending_start_time=std::numeric_limits<float>::quiet_NaN();
        if (bad==2) seed.countdown_begin_time=std::numeric_limits<float>::infinity();
        if (bad==3) seed.countdown_begin_time=-1.0F;
        if (bad==4) seed.waiting_for=-1;
        if (bad==5) seed.waiting_to_ready=-1;
        invalid([&] { PendingMatch rejected(rules(),seed); });
    }
}

TEST_CASE("PendingMatch invalid inputs leave state latch and last valid evaluation clock untouched") {
    PendingMatch model(rules(),initial());
    REQUIRE_FALSE(model.evaluate(inputs(5.0F)).start_requested);
    const auto before=model.snapshot();
    for (unsigned bad=0;bad<8;++bad) {
        CAPTURE(bad);
        auto event=inputs(6.0F);
        if (bad==0) event.now=std::numeric_limits<float>::quiet_NaN();
        if (bad==1) event.now=std::numeric_limits<float>::infinity();
        if (bad==2) event.now=-1.0F;
        if (bad==3) event.now=4.0F;
        if (bad==4) event.num_players=-1;
        if (bad==5) event.num_travelling_players=-1;
        if (bad==6) { event.num_players=std::numeric_limits<std::int32_t>::max();event.num_travelling_players=1; }
        if (bad==7) event.now=std::numeric_limits<float>::max();
        invalid([&] { model.evaluate(event); });
        CHECK(model.snapshot() == before);
    }
    CHECK_FALSE(model.evaluate(inputs(6.0F)).start_requested);
    CHECK(model.snapshot().countdown_remaining == 9);
    CHECK(model.evaluate(inputs(15.0F)).start_requested);
}

TEST_CASE("PendingMatch valid early returns still advance monotonic world clock") {
    PendingMatch model(rules(),initial());
    auto event=inputs(20.0F);event.tracking_created=false;
    CHECK_FALSE(model.evaluate(event).start_requested);
    CHECK(model.snapshot() == initial());
    invalid([&] { model.evaluate(inputs(19.0F)); });
    CHECK(model.snapshot() == initial());
    CHECK_FALSE(model.evaluate(inputs(20.0F)).start_requested);
    CHECK(model.snapshot().countdown_begin_time == 20.0F);
}

TEST_CASE("PendingMatch float to int overflow rejects instead of invoking undefined conversion") {
    auto config=rules();config.countdown_seconds=std::numeric_limits<std::int32_t>::max();
    PendingMatch countdown(config,initial());
    invalid([&] { countdown.evaluate(inputs(0.0F)); });
    CHECK(countdown.snapshot() == initial());
    PendingMatch percent(rules(),initial());
    auto event=inputs(5.0F,{});event.num_players=std::numeric_limits<std::int32_t>::max();
    auto all_rules=rules();all_rules.min_percent_ready=1.0F;
    PendingMatch ceil_model(all_rules,initial());
    invalid([&] { ceil_model.evaluate(event); });
    CHECK(ceil_model.snapshot() == initial());
    invalid([&] { percent.evaluate(inputs(std::numeric_limits<float>::max())); });
    CHECK(percent.snapshot() == initial());
}
