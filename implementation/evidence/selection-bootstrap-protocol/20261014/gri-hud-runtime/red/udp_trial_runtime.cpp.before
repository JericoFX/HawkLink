#include "hawken/udp_trial_runtime.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#endif

namespace hawken::udp {
namespace {
[[noreturn]] void startup_failed() {
    throw std::runtime_error("UDP trial runtime could not start.");
}
bool random_fill(std::span<std::uint8_t> bytes) noexcept {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    if (bytes.empty()) return true;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1;
#endif
}
std::uint32_t random_challenge() {
    std::array<std::uint8_t, 4> bytes{};
    if (!random_fill(bytes)) startup_failed();
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    return value;
}
std::int64_t expiry_seconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) startup_failed();
    const long double whole = std::floor(static_cast<long double>(seconds));
    // Strict 2^63 boundary also works where long double has double precision.
    if (whole >= 9223372036854775808.0L) startup_failed();
    return static_cast<std::int64_t>(whole);
}
std::int64_t utc_seconds() {
    const long double seconds = std::chrono::duration<long double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 253402300799.0L) startup_failed();
    return static_cast<std::int64_t>(std::floor(seconds));
}
game::ConnectionConfig connection_config() {
    game::ConnectionConfig config{}; // Zero material is a skeleton, never a Context.
    // CONDITIONAL fresh original class/CDO baseline; no native runtime proof.
    config.transport = {0, 0xffffffffU, 0xffffffffU, 0, 0, false, 512,
        {64, 64, 64, 65536, 64, 64}, 120.0};
    config.endian = control::Endian::little;
    config.challenge = random_challenge();
    config.server_min = 9188;
    config.server_max = 154767;
    config.reply_flags = {false, false};
    config.limits = {560, 16, 64, 35840};
    config.login = {{512, 256}, random_challenge(), 30000};
    return config;
}
}

bool validate_trial_config(const UdpTrialConfig& config) noexcept {
    const auto& guid = config.server_guid;
    if (guid.size() != 36 || config.region.empty() || config.region.size() > 64) return false;
    bool nonzero = false;
    for (std::size_t index = 0; index < guid.size(); ++index) {
        const char value = guid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') return false;
        } else {
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
            nonzero = nonzero || value != '0';
        }
    }
    if (!nonzero) return false;
    for (const char value : config.region)
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-')) return false;
    return true;
}

struct UdpTrialRuntime::Impl {
    const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    const std::optional<game::WorldPublication> publication;
    const std::optional<InitialControllerPlan> controller;
    const std::optional<InitialPawnPlan> pawn;
    const std::optional<InitialPriPlan> pri;
    const std::optional<InitialActorLinksPlan> links;
    const std::optional<StartMechDispatchPlan> start_mech;
    const std::optional<StartMechSource> start_mech_source;
    const std::optional<SpawnValidationDispatchPlan> spawn_validation;
    const std::optional<SpawnValidationSource> spawn_validation_source;
    const std::optional<SpawnAdmissionDispatchPlan> spawn_admission;
    const std::optional<PriIdentityDispatchPlan> pri_identity;
    std::unique_ptr<LocalUdpOwner> owner;
    std::unique_ptr<assignment::SessionAssignmentService> service;

    double monotonic() const {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
        if (!std::isfinite(seconds) || seconds < 0) startup_failed();
        return seconds;
    }
    Impl(auth::AuthService& auth, const UdpTrialConfig& config)
        : publication(config.publication), controller(config.controller), pawn(config.pawn), pri(config.pri),
          links(config.links), start_mech(config.start_mech), start_mech_source(config.start_mech_source),
          spawn_validation(config.spawn_validation), spawn_validation_source(config.spawn_validation_source),
          spawn_admission(config.spawn_admission), pri_identity(config.pri_identity) {
        try {
            owner = std::make_unique<LocalUdpOwner>(config.port, connection_config(),
                [this] { return monotonic(); }, std::chrono::milliseconds(20));
            assignment::Destination destination{config.server_guid, "127.0.0.1",
                static_cast<std::int32_t>(owner->port()), config.region, trial_game_version};
            service = std::make_unique<assignment::SessionAssignmentService>(auth, std::move(destination),
                assignment::AssignmentPolicy{900, 0, 0},
                [this] { return expiry_seconds(monotonic()); }, utc_seconds, random_fill,
                [this](const assignment::Destination& target) { return owner->destination_bound(target); },
                [this](const assignment::SessionProvision& provision) { return owner->handoff(provision); });
            LocalUdpOwner::PublicationProvider publish;
            if (publication) {
                // No asset I/O or identity decision here. Owner invokes this
                // only after Response validation and rechecks the active lease
                // before publishing through its persistent GameConnection.
                publish = [this](const session::SessionId&, const control::posthello::Login&) {
                    return publication;
                };
            }
            LocalUdpOwner::StartMechProvider materialize;
            if (start_mech) {
                materialize=[this](const session::SessionId& id,const wire::rpc::StartMechArguments& request,
                                  std::uint8_t previous_setup_id)
                    ->std::optional<wire::mech::SMechSetupStruct> {
                    // Owner invokes this only for a decoded request, outside Use.
                    // Reading an existing owned snapshot never seeds/grants items.
                    const auto account=service->account_for_udp(id);
                    if (!account) return std::nullopt;
                    const auto snapshot=start_mech_source->read_effective(*account,request.mech_instance_id);
                    if (snapshot.items[0].instance_id!=request.mech_instance_id) return std::nullopt;
                    // Original handler narrows BOTH signed arguments to BYTE first.
                    const auto primary=static_cast<std::uint8_t>(static_cast<std::uint32_t>(request.primary_weapon));
                    const auto holo=static_cast<std::uint8_t>(static_cast<std::uint32_t>(request.holo_taunt_type));
                    auto value=inventory::materialize_starter_mech(snapshot,start_mech_source->expected_catalogue,
                        {primary,holo,previous_setup_id,std::nullopt,true});
                    const auto current=service->account_for_udp(id);
                    if (!current || *current!=*account) return std::nullopt;
                    return value;
                };
            }
            LocalUdpOwner::SpawnSelectionValidator validate_selection;
            if (spawn_validation) {
                validate_selection=[this](const session::SessionId& id,
                    const wire::rpc::StartMechArguments& request,std::uint8_t setup_id) {
                    const SpawnSelectionValidationResult failed{{2,2,2,2,2},0};
                    try {
                        const auto account=service->account_for_udp(id);
                        if (!account || setup_id==255) return failed;
                        // This reader supplies the complete atomic owned nine-role
                        // graph and effective baseline, never an autogranted preset.
                        const auto snapshot=start_mech_source->read_effective(*account,request.mech_instance_id);
                        if (snapshot.items[0].instance_id!=request.mech_instance_id) return failed;
                        const auto primary=static_cast<std::uint8_t>(static_cast<std::uint32_t>(request.primary_weapon));
                        const auto holo=static_cast<std::uint8_t>(static_cast<std::uint32_t>(request.holo_taunt_type));
                        const auto previous=static_cast<std::uint8_t>(setup_id==0 ? 254 : setup_id-1);
                        const auto selected=inventory::materialize_starter_mech(snapshot,
                            start_mech_source->expected_catalogue,{primary,holo,previous,std::nullopt,true});
                        if (selected.setup_id!=setup_id || !spawn_validation_source->permanent_starter_instances)
                            return failed;
                        // The finite grant/materializer validates root ownership,
                        // role IDs and the selected primary's root/child relation.
                        // Permanence is explicit local policy, not a native expiry claim.
                        const auto achievements=spawn_validation_source->read_achievement_ids(*account);
                        const auto current=service->account_for_udp(id);
                        if (!current || *current!=*account) return failed;
                        // No achievement catalogue/award engine is configured here.
                        // Empty means a completed scoped SQL read; failures never do.
                        if (!achievements.empty()) return failed;
                        return SpawnSelectionValidationResult{{3,3,3,3,3},7};
                    } catch (...) { return failed; }
                };
            }
            // Start last: all callback targets are complete before the worker.
            owner->start([this](const session::SessionId& id) { return service->authorize_udp(id); },
                [this](const session::SessionId& id, const control::posthello::RawString& response) {
                    return service->validate_login_response(id, response);
                }, std::move(publish), controller, pawn, pri, links,start_mech,std::move(materialize),
                spawn_validation,std::move(validate_selection),spawn_admission,pri_identity);
        } catch (...) {
            // If start partially succeeded, join before unwinding service/this.
            stop();
            startup_failed();
        }
    }
    ~Impl() { stop(); }
    void stop() noexcept { if (owner) owner->stop(); }
};

UdpTrialRuntime::UdpTrialRuntime(auth::AuthService& auth, const UdpTrialConfig& config) {
    if (!validate_trial_config(config))
        throw std::invalid_argument("Invalid UDP trial configuration.");
    if (config.links && (!config.publication || !config.controller || !config.pri || !config.pawn))
        startup_failed();
    if (static_cast<bool>(config.start_mech)!=static_cast<bool>(config.start_mech_source))
        startup_failed();
    if (config.start_mech && (!config.publication || !config.controller || !config.pri ||
        !config.pawn || !config.links || !config.start_mech_source->read_effective))
        startup_failed();
    if (static_cast<bool>(config.spawn_validation)!=static_cast<bool>(config.spawn_validation_source))
        startup_failed();
    if (config.spawn_validation && (!config.publication || !config.controller || !config.pri ||
        !config.pawn || !config.links || !config.start_mech || !config.start_mech_source ||
        !config.start_mech_source->read_effective || !config.spawn_validation_source->read_achievement_ids ||
        !config.spawn_validation_source->permanent_starter_instances))
        startup_failed();
    if (config.spawn_admission && (!config.spawn_validation || !config.spawn_validation_source ||
        !config.publication || !config.controller || !config.pri || !config.pawn || !config.links ||
        !config.start_mech || !config.start_mech_source))
        startup_failed();
    if (config.pri_identity) {
        const auto& identity = *config.pri_identity;
        const auto& eligibility = identity.eligibility;
        if (!config.publication || !config.controller || !config.pri || !config.pawn || !config.links ||
            identity.exclusive_bound == 0 || identity.field_id >= identity.exclusive_bound ||
            (identity.integer_order != control::Endian::little && identity.integer_order != control::Endian::big) ||
            !eligibility.primary_package_map || !eligibility.pri_cache_correct ||
            !eligibility.unique_id_property_eligible || !eligibility.unique_net_id_serializer_correct ||
            !eligibility.integer_archive_order_correct)
            startup_failed();
    }
    try { impl_ = std::make_unique<Impl>(auth, config); }
    catch (...) { startup_failed(); }
}
UdpTrialRuntime::~UdpTrialRuntime() { stop(); }
bool UdpTrialRuntime::running() const noexcept { return impl_->owner->running(); }
std::uint16_t UdpTrialRuntime::port() const noexcept { return impl_->owner->port(); }
assignment::SessionAssignmentService& UdpTrialRuntime::assignment() { return *impl_->service; }
LocalUdpDiagnostics UdpTrialRuntime::diagnostics() const noexcept { return impl_->owner->diagnostics(); }
void UdpTrialRuntime::stop() noexcept { if (impl_) impl_->stop(); }
}
