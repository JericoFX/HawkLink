#include "hawken/match_readiness.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hawken::game {
namespace {
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid match readiness.");
}
bool valid_time(float value) {
    return std::isfinite(value) && value >= 0.0F;
}
std::int32_t checked_int(float value) {
    // Compare in double: float(INT32_MAX) rounds UP to 2^31, outside int32.
    if (!std::isfinite(value) ||
        static_cast<double>(value) < std::numeric_limits<std::int32_t>::min() ||
        static_cast<double>(value) > std::numeric_limits<std::int32_t>::max())
        invalid();
    return static_cast<std::int32_t>(value);
}
}

PendingMatch::PendingMatch(PendingMatchRules rules, PendingMatchState initial_state)
    : rules_(rules), state_(initial_state) {
    if (rules_.min_players < 0 || !std::isfinite(rules_.min_percent_ready) ||
        rules_.min_percent_ready < 0.0F || rules_.min_percent_ready > 1.0F ||
        rules_.min_delay_seconds < 0 || rules_.max_delay_seconds < 0 ||
        rules_.countdown_seconds < 0 || !valid_time(rules_.max_wait_for_travelling_seconds) ||
        !valid_time(state_.pending_start_time) || !valid_time(state_.countdown_begin_time) ||
        state_.waiting_for < 0 || state_.waiting_to_ready < 0)
        invalid();
}

PendingMatchEvaluation PendingMatch::evaluate(const PendingMatchInputs& inputs) {
    if (!valid_time(inputs.now) || inputs.now < state_.pending_start_time ||
        (last_valid_evaluation_ && inputs.now < *last_valid_evaluation_) ||
        inputs.num_players < 0 || inputs.num_travelling_players < 0 ||
        inputs.num_players > std::numeric_limits<std::int32_t>::max() - inputs.num_travelling_players)
        invalid();

    auto next = state_;
    const auto commit = [&](bool start_requested) {
        state_ = next;
        last_valid_evaluation_ = inputs.now;
        if (start_requested) start_request_emitted_ = true;
        return PendingMatchEvaluation{start_requested};
    };
    if (start_request_emitted_) return commit(false);

    // Deathmatch checks every concrete controller before the PendingMatch
    // tracking/travel gates, independently of whether it counts as a player.
    for (const auto& player : inputs.controllers)
        if (!player.online_synced || player.purchasing || player.saving)
            return commit(false);
    if (!inputs.tracking_created || inputs.pending_quit) return commit(false);

    const auto population = inputs.num_players + inputs.num_travelling_players;
    const float elapsed = inputs.now - next.pending_start_time;
    const auto needed_to_ready = [&]() -> std::int32_t {
        if (population == 0) return 1;
        if (inputs.match_has_begun || !rules_.players_must_be_ready ||
            rules_.min_players == 0 || rules_.min_percent_ready == 0.0F) {
            next.waiting_for = 0;
            next.waiting_to_ready = 0;
            return 0;
        }
        const auto population_gap = std::max(rules_.min_players - population, std::int32_t{0});
        if (population_gap > 0) {
            next.waiting_for = population_gap;
            return population_gap; // Keep the old waiting_to_ready value.
        }
        next.waiting_for = 0;
        const float product = static_cast<float>(inputs.num_players) * rules_.min_percent_ready;
        const auto minimum_ready = checked_int(std::ceil(product));
        std::int32_t ready = 0;
        for (const auto& player : inputs.controllers) {
            if (player.is_player && player.waiting_player && player.ready) {
                if (ready == std::numeric_limits<std::int32_t>::max()) invalid();
                ++ready;
            }
        }
        next.waiting_to_ready = std::max(minimum_ready - ready, std::int32_t{0});
        return next.waiting_to_ready;
    };
    const auto deactivate = [&]() {
        if (next.timer_active) {
            next.timer_active = false;
            next.countdown_begin_time = 100000000000.0F;
            next.countdown_remaining = -1;
        }
    };

    if (inputs.num_travelling_players != 0 &&
        !(elapsed > rules_.max_wait_for_travelling_seconds)) {
        needed_to_ready();
        deactivate();
        return commit(false);
    }

    bool eligible = false;
    if (!rules_.players_must_be_ready) {
        eligible = population != 0;
    } else {
        const auto elapsed_seconds = checked_int(elapsed);
        eligible = (rules_.max_delay_seconds > 0 && elapsed_seconds >= rules_.max_delay_seconds)
            || needed_to_ready() == 0;
    }
    if (!eligible) {
        deactivate();
        return commit(false);
    }
    next.waiting_for = 0;
    next.waiting_to_ready = 0;
    if (elapsed < static_cast<float>(rules_.min_delay_seconds)) {
        const float remaining_minimum = static_cast<float>(rules_.min_delay_seconds) - elapsed;
        const float remaining = static_cast<float>(rules_.countdown_seconds) + remaining_minimum;
        next.countdown_remaining = checked_int(remaining);
        return commit(false);
    }
    if (!inputs.waiting_to_start_match) return commit(true);
    if (!next.timer_active) {
        next.timer_active = true;
        next.countdown_begin_time = inputs.now;
        next.countdown_remaining = checked_int(static_cast<float>(rules_.countdown_seconds)
            - (inputs.now - next.countdown_begin_time));
        return commit(false);
    }
    const float countdown_elapsed = inputs.now - next.countdown_begin_time;
    if (countdown_elapsed >= static_cast<float>(rules_.countdown_seconds))
        return commit(true);
    next.countdown_remaining = checked_int(static_cast<float>(rules_.countdown_seconds) - countdown_elapsed);
    return commit(false);
}
}
