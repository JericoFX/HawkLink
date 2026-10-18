#include "hawken/weapon_fire_state.hpp"
#include <algorithm>
#include <cmath>

namespace hawken::game::weapon_fire {
namespace {
bool valid_authority(const Config& config,const Snapshot& state,
                     const Authority& authority,double now) {
    return config.scope.spawn_revision!=0 && config.scope.actor_channel!=0 &&
        config.scope.actor_channel<=2047 &&
        std::any_of(config.scope.session_id.begin(),config.scope.session_id.end(),
            [](auto byte) { return byte!=0; }) &&
        std::any_of(config.supported_hands.begin(),config.supported_hands.end(),
            [](bool present) { return present; }) &&
        authority.scope==config.scope && authority.authenticated &&
        authority.actor_owned && authority.instigator_valid &&
        authority.pawn.spawn_revision==config.scope.spawn_revision &&
        std::isfinite(now) && now>=0.0 && (!state.last_time || now>=*state.last_time);
}
bool append(Step& step,ActionKind kind,std::size_t hand) {
    if (step.action_count>=step.actions.size()) return false;
    step.actions[step.action_count++]={kind,static_cast<std::uint8_t>(hand)};
    return true;
}
bool update_permission(const Config& config,const Authority& authority,
                       Snapshot& next,Step& step) {
    const bool enabled=evaluate_pawn_combat(authority.pawn,
        config.scope.spawn_revision,authority.client_shutdown).local_can_fire;
    if (enabled==next.enabled) return true;
    for (std::size_t hand=0;hand<config.supported_hands.size();++hand) {
        if (!config.supported_hands[hand]) continue;
        if (enabled) {
            if (!next.pending_fire[hand]) continue;
            if (!append(step,ActionKind::begin_fire,hand)) return false;
            next.firing[hand]=true;
        } else {
            // Active.ShutdownWeapons ends every present hand, without clearing
            // PendingFire. A later restart can resume the retained intention.
            if (!append(step,ActionKind::end_fire,hand)) return false;
            next.firing[hand]=false;
        }
    }
    next.enabled=enabled;
    return true;
}
}

FireState::FireState(Config config) : config_(config) {}
Step FireState::apply(const Result& request,const Authority& authority,double now) {
    const auto hand=static_cast<std::size_t>(request.fire_mode);
    if (!valid_authority(config_,state_,authority,now) ||
        hand>=config_.supported_hands.size() || !config_.supported_hands[hand]) return {};
    switch (request.kind) {
    case Kind::start_fire:
        if (!authority.pawn.alive || !authority.pawn.spawn_active ||
            request.recent_fire.has_value()) return {};
        break;
    case Kind::stop_fire:
        if (request.recent_fire.has_value()) return {};
        break;
    case Kind::stop_fire_extended:
        if (!request.recent_fire.has_value()) return {};
        break;
    default: return {};
    }

    auto next=state_;
    Step step;
    if (!update_permission(config_,authority,next,step)) return {};
    if (request.kind==Kind::start_fire) {
        // BeginFire always records intent, even while effective firing is vetoed.
        next.pending_fire[hand]=true;
        if (next.enabled) {
            if (!append(step,ActionKind::begin_fire,hand)) return {};
            next.firing[hand]=true;
        }
        if (hand<next.stop_deadlines.size()) next.stop_deadlines[hand].reset();
    } else if (request.kind==Kind::stop_fire_extended &&
               *request.recent_fire && hand<next.stop_deadlines.size()) {
        const double deadline=now+static_cast<double>(0.2F);
        if (!std::isfinite(deadline)) return {};
        next.stop_deadlines[hand]=deadline;
    } else {
        // EndFire is an invocation, not an edge-only notification. Keep any
        // previously armed timer: the original EndFire does not ClearTimer.
        next.pending_fire[hand]=false;
        next.firing[hand]=false;
        if (!append(step,ActionKind::end_fire,hand)) return {};
    }
    next.last_time=now;
    state_=next;
    step.accepted=true;
    return step;
}
Step FireState::tick(const Authority& authority,double now) {
    if (!valid_authority(config_,state_,authority,now)) return {};
    auto next=state_;
    Step step;
    if (!update_permission(config_,authority,next,step)) return {};
    for (std::size_t hand=0;hand<next.stop_deadlines.size();++hand) {
        if (!next.stop_deadlines[hand] || now<*next.stop_deadlines[hand]) continue;
        next.stop_deadlines[hand].reset();
        next.pending_fire[hand]=false;
        next.firing[hand]=false;
        if (!append(step,ActionKind::end_fire,hand)) return {};
    }
    next.last_time=now;
    state_=next;
    step.accepted=true;
    return step;
}
const Snapshot& FireState::snapshot() const noexcept { return state_; }
}
