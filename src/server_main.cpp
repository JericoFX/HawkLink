#include "hawken/server_http.hpp"
#include "hawken/http_diagnostics.hpp"
#include "hawken/auth_service.hpp"
#include "hawken/sync_service.hpp"
#include "hawken/inventory_service.hpp"
#include "hawken/menu_sync_service.hpp"
#include "hawken/steam_local_auth.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include "hawken/local_tcp_probe.hpp"
#include "hawken/wreckage_trial.hpp"
#include "hawken/sha256.hpp"
#include "hawken/master_directory.hpp"
#include <cpp-httplib/httplib.h>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

namespace {
std::atomic<bool> shutdown_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
    "Signal handlers require a lock-free shutdown flag.");

void request_shutdown(int) noexcept {
    shutdown_requested.store(true, std::memory_order_relaxed);
}

void print_help() {
    std::cout << "Usage: hawken-server [--port 1..65535] | --help\n"
        "Local backend bootstrap listener on 127.0.0.1; default port 80.\n"
        "Optional local Steam test: --steam-local --database ABSOLUTE_FILE\n"
        "  --server-id ID --diagnostics ABSOLUTE_FILE --stop-file ABSOLUTE_FILE\n"
        "  [--starter-profile ABSOLUTE_FILE] for the explicit local starter policy.\n"
        "  Optional UDP trial: --udp-port 1..65535 --udp-server-guid UUID --udp-region REGION\n"
        "  All three UDP options require --steam-local; handshake only, no hosted world.\n"
        "  UDP trials also bind the original TCP establishment probe on 127.0.0.1:15520.\n"
        "  Optional world publication trial: --udp-world-root ABSOLUTE_GAME_ROOT\n"
        "  Requires all UDP options; verifies pinned original Wreckage assets before startup.\n"
        "Original Steam user and AppID 271290 license required; one auth attempt.\n"
        "Game admission and gameplay remain unavailable.\n";
}

struct Options {
    int port = 80;
    bool steam_local = false;
    std::filesystem::path database, diagnostics, stop_file, starter_profile;
    std::string server_id;
    std::optional<hawken::udp::UdpTrialConfig> udp_trial;
    std::filesystem::path udp_world_root;
};

bool safe_data_path(const std::filesystem::path& path) {
    if (!path.is_absolute()) return false;
    const auto resolved = std::filesystem::weakly_canonical(path);
#ifdef _WIN32
    const auto original = std::filesystem::weakly_canonical(
        L"F:\\SteamLibrary\\steamapps\\common\\Hawken");
    auto candidate = resolved.begin();
    bool inside_game = true;
    for (const auto& component : original) {
        if (candidate == resolved.end()
            || CompareStringOrdinal(component.c_str(), -1, candidate->c_str(), -1, TRUE) != CSTR_EQUAL) {
            inside_game = false;
            break;
        }
        ++candidate;
    }
    if (inside_game) return false;
#endif
    return !resolved.empty();
}

bool same_path(const std::filesystem::path& first, const std::filesystem::path& second) {
#ifdef _WIN32
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return first == second;
#endif
}

std::filesystem::path utf8_path(std::string_view value) {
    std::u8string utf8(value.size(), u8'\0');
    for (std::size_t index = 0; index < value.size(); ++index)
        utf8[index] = static_cast<char8_t>(static_cast<unsigned char>(value[index]));
    return std::filesystem::path(utf8);
}

bool parse_options(int argc, char* argv[], Options& options) {
    bool port_seen = false, database_seen = false, id_seen = false;
    bool diagnostics_seen = false, stop_seen = false, profile_seen = false;
    bool udp_port_seen = false, udp_guid_seen = false, udp_region_seen = false, world_seen = false;
    hawken::udp::UdpTrialConfig udp_trial{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view key(argv[index]);
        if (key == "--steam-local") {
            if (options.steam_local) return false;
            options.steam_local = true;
            continue;
        }
        if (index + 1 >= argc) return false;
        const std::string_view value(argv[++index]);
        if (value.empty()) return false;
        if (key == "--port" && !port_seen) {
            port_seen = true;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.port, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || options.port < 1 || options.port > 65535) return false;
        } else if (key == "--udp-port" && !udp_port_seen) {
            udp_port_seen = true;
            int port = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || port < 1 || port > 65535) return false;
            udp_trial.port = static_cast<std::uint16_t>(port);
        } else if (key == "--udp-server-guid" && !udp_guid_seen) {
            udp_guid_seen = true;
            udp_trial.server_guid = value;
        } else if (key == "--udp-region" && !udp_region_seen) {
            udp_region_seen = true;
            udp_trial.region = value;
        } else if (key == "--udp-world-root" && !world_seen) {
            world_seen = true;
            options.udp_world_root = utf8_path(value);
        } else if (key == "--database" && !database_seen) {
            database_seen = true;
            options.database = utf8_path(value);
        } else if (key == "--server-id" && !id_seen) {
            id_seen = true;
            options.server_id = value;
        } else if (key == "--diagnostics" && !diagnostics_seen) {
            diagnostics_seen = true;
            options.diagnostics = utf8_path(value);
        } else if (key == "--stop-file" && !stop_seen) {
            stop_seen = true;
            options.stop_file = utf8_path(value);
        } else if (key == "--starter-profile" && !profile_seen) {
            profile_seen = true;
            options.starter_profile = utf8_path(value);
        } else return false;
    }
    if (udp_port_seen || udp_guid_seen || udp_region_seen) {
        if (!options.steam_local || !udp_port_seen || !udp_guid_seen || !udp_region_seen
            || !hawken::udp::validate_trial_config(udp_trial)) return false;
        options.udp_trial = std::move(udp_trial);
    }
    const bool any_auth_parameter = database_seen || id_seen || diagnostics_seen || stop_seen || profile_seen;
    if (world_seen && (!options.udp_trial || !options.udp_world_root.is_absolute())) return false;
    if (!options.steam_local) return !any_auth_parameter;
    if (!database_seen || !id_seen || !diagnostics_seen || !stop_seen
        || options.server_id.empty() || options.server_id.size() > 64
        || options.server_id == "." || options.server_id == "..") return false;
    for (const char value : options.server_id) {
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.')) return false;
    }
    if (!safe_data_path(options.database) || !safe_data_path(options.diagnostics)
        || !safe_data_path(options.stop_file)) return false;
    options.database = std::filesystem::weakly_canonical(options.database);
    options.diagnostics = std::filesystem::weakly_canonical(options.diagnostics);
    options.stop_file = std::filesystem::weakly_canonical(options.stop_file);
    if (same_path(options.database, options.diagnostics) || same_path(options.database, options.stop_file)
        || same_path(options.diagnostics, options.stop_file)) return false;
    if (profile_seen) {
        if (!safe_data_path(options.starter_profile)
            || !std::filesystem::is_regular_file(options.starter_profile)) return false;
        options.starter_profile = std::filesystem::weakly_canonical(options.starter_profile);
        if (same_path(options.starter_profile, options.database)
            || same_path(options.starter_profile, options.diagnostics)
            || same_path(options.starter_profile, options.stop_file)) return false;
    }
    return !std::filesystem::exists(options.diagnostics) && !std::filesystem::exists(options.stop_file);
}

struct StarterPolicy {
    hawken::inventory::StarterCatalogueIds ids;
    hawken::inventory::StarterBalanceProfile balance;
    std::string offer_id;
};

StarterPolicy read_starter_policy(const std::filesystem::path& path) {
    constexpr std::size_t limit = 16384;
    std::array<char, limit + 1> text{};
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Invalid starter profile.");
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto length = input.gcount();
    if (input.bad() || length <= 0 || length > static_cast<std::streamsize>(limit))
        throw std::invalid_argument("Invalid starter profile.");
    using nlohmann::json;
    const auto root = json::parse(text.data(), text.data() + length,
        [](int depth, json::parse_event_t, json&) {
            if (depth > 8) throw std::invalid_argument("Invalid starter profile.");
            return true;
        });
    if (!root.is_object() || root.size() != (root.contains("FtuxTest") ? 8U : 7U))
        throw std::invalid_argument("Invalid starter profile.");
    const auto& types = root.at("Types");
    if (!types.is_object() || types.size() != 9)
        throw std::invalid_argument("Invalid starter profile.");
    StarterPolicy policy;
    policy.ids = {types.at("Mech").get<std::string>(), types.at("Primary").get<std::string>(),
        types.at("Secondary").get<std::string>(), types.at("Upper").get<std::string>(),
        types.at("Middle").get<std::string>(), types.at("Arms").get<std::string>(),
        types.at("Lower").get<std::string>(), types.at("Drone").get<std::string>(),
        types.at("Ability").get<std::string>()};
    policy.balance.version = root.at("Version").get<std::string>();
    const auto weapon = [](const json& values) -> hawken::inventory::WeaponBalance {
        if (!values.is_object() || values.size() != 11)
            throw std::invalid_argument("Invalid starter profile.");
        const auto number = [&values](const char* key) {
            const auto& value = values.at(key);
            if (!value.is_number()) throw std::invalid_argument("Invalid starter profile.");
            return value.get<float>();
        };
        return {number("HeatPerShot"), number("Spread"), number("RefireRate"), number("WeaponRange"),
            number("InstantHitDamage"), number("AimHelpdist"), number("DamageForUI"),
            number("RefireRateForUI"), number("HeatForUI"), number("EffectiveRangeForUI"), number("AccuracyForUI")};
    };
    policy.balance.primary = weapon(root.at("PrimaryWeapon"));
    policy.balance.secondary = weapon(root.at("SecondaryWeapon"));
    const auto& projectile = root.at("SecondaryProjectile");
    if (!projectile.is_object() || projectile.size() != 7)
        throw std::invalid_argument("Invalid starter profile.");
    const auto projectile_number = [&projectile](const char* key) {
        const auto& value = projectile.at(key);
        if (!value.is_number()) throw std::invalid_argument("Invalid starter profile.");
        return value.get<float>();
    };
    policy.balance.secondary_projectile = {projectile_number("Speed"),
        projectile_number("MaxSpeed"), projectile_number("AccelRate"),
        projectile_number("Damage"), projectile_number("DamageRadius"),
        projectile_number("InnerDamageRadius"), projectile_number("MinDamage")};
    const auto& coolant = root.at("Coolant");
    if (!coolant.is_object() || coolant.size() != 3)
        throw std::invalid_argument("Invalid starter profile.");
    const auto coolant_number = [&coolant](const char* key) {
        const auto& value = coolant.at(key);
        if (!value.is_number()) throw std::invalid_argument("Invalid starter profile.");
        return value.get<float>();
    };
    policy.balance.coolant = {coolant_number("RegenerationSeconds"), coolant_number("ModeActiveTime"),
        coolant_number("HeatReductionRate")};
    if (root.contains("FtuxTest"))
        policy.balance.ftux_test = hawken::inventory::parse_ftux_test(root.at("FtuxTest"));
    // Validate the same type/balance rules before Steam initialization or SQL migration.
    static_cast<void>(hawken::inventory::starter_catalogue_result(policy.ids, policy.balance));
    policy.offer_id = root.at("StarterOfferGuid").get<std::string>();
    if (policy.offer_id.size() != 36) throw std::invalid_argument("Invalid starter profile.");
    bool nonzero = false;
    for (std::size_t i = 0; i < policy.offer_id.size(); ++i) {
        const char c = policy.offer_id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') throw std::invalid_argument("Invalid starter profile.");
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                throw std::invalid_argument("Invalid starter profile.");
            nonzero = nonzero || c != '0';
        }
    }
    if (!nonzero) throw std::invalid_argument("Invalid starter profile.");
    if (policy.balance.ftux_test && policy.balance.ftux_test->globals_guid == policy.offer_id)
        throw std::invalid_argument("Invalid starter profile.");
    for (const auto& type : types)
        if (type.get<std::string>() == policy.offer_id)
            throw std::invalid_argument("Invalid starter profile.");
    return policy;
}

class Diagnostics {
public:
    explicit Diagnostics(const std::filesystem::path& file) {
        if (file.empty()) stream_ = &std::cout;
        else {
            // CLI preflight rejects an existing file. Append avoids truncation if
            // another process creates it between validation and open.
            file_.open(file, std::ios::out | std::ios::app);
            if (!file_) throw std::runtime_error("Local diagnostics could not open.");
            stream_ = &file_;
        }
    }
    std::ostream& stream() { return *stream_; }
    void steam(const hawken::auth::LocalSteamObservation& observation) {
        using Phase = hawken::auth::LocalSteamPhase;
        const char* phase = "other";
        switch (observation.phase) {
        case Phase::startup: phase = "startup"; break;
        case Phase::begin: phase = "begin"; break;
        case Phase::callback143: phase = "callback143"; break;
        case Phase::license: phase = "license"; break;
        case Phase::session_end: phase = "session-end"; break;
        case Phase::shutdown: phase = "shutdown"; break;
        case Phase::context_lost: phase = "context_lost"; break;
        case Phase::worker_finished: phase = "worker_finished"; break;
        }
        const char* context_loss = "none";
        switch (observation.context_loss) {
        case hawken::auth::LocalSteamContextLoss::none: break;
        case hawken::auth::LocalSteamContextLoss::pipe: context_loss = "pipe"; break;
        case hawken::auth::LocalSteamContextLoss::user: context_loss = "user"; break;
        case hawken::auth::LocalSteamContextLoss::logged_off: context_loss = "logged_off"; break;
        }
        const char* worker_finish = "none";
        switch (observation.worker_finish) {
        case hawken::auth::LocalSteamWorkerFinish::none: break;
        case hawken::auth::LocalSteamWorkerFinish::normal: worker_finish = "normal"; break;
        case hawken::auth::LocalSteamWorkerFinish::exception: worker_finish = "exception"; break;
        }
        std::osyncstream(*stream_) << "steam phase=" << phase
            << " available=" << observation.available << " begin_ok=" << observation.begin_ok
            << " packed20=" << observation.packed20 << " identity_match=" << observation.identity_match
            << " response_ok=" << observation.response_ok << " license_ok=" << observation.license_ok
            << " gs_shutdown=" << observation.gs_shutdown_returned
            << " gs_pipe_zero=" << observation.gs_pipe_zero
            << " context_loss=" << context_loss << " worker_finish=" << worker_finish << '\n' << std::flush;
    }
    void udp(const hawken::udp::LocalUdpDiagnostics& observation) {
        std::osyncstream(*stream_)
            << "udp authenticated_datagrams=" << observation.authenticated_datagrams
            << " hello_validated=" << observation.hello_validated
            << " login_received=" << observation.login_received
            << " login_response_validated=" << observation.login_response_validated
            << " world_published=" << observation.world_published
            << " join_requested=" << observation.join_requested
            << " controller_sent=" << observation.controller_sent
            << " pri_sent=" << observation.pri_sent
            << " pawn_sent=" << observation.pawn_sent
            << " controller_open_acked=" << observation.controller_open_acked
            << " pri_open_acked=" << observation.pri_open_acked
            << " pawn_open_acked=" << observation.pawn_open_acked
            << " actor_links_sent=" << observation.actor_links_sent
            << " pri_identity_sent=" << observation.pri_identity_sent
            << " pri_identity_acked=" << observation.pri_identity_acked
            << " gri_sent=" << observation.gri_sent
            << " gri_open_acked=" << observation.gri_open_acked
            << " game_class_sent=" << observation.game_class_sent
            << " game_class_acked=" << observation.game_class_acked
            << " hud_sent=" << observation.hud_sent
            << " hud_acked=" << observation.hud_acked
            << " ready_rpc_received=" << observation.ready_rpc_received
            << " pri_ready_sent=" << observation.pri_ready_sent
            << " pri_ready_acked=" << observation.pri_ready_acked
            << " match_start_requested=" << observation.match_start_requested
            << " gri_match_begun_sent=" << observation.gri_match_begun_sent
            << " gri_match_begun_acked=" << observation.gri_match_begun_acked
            << " spawn_validation_requested=" << observation.spawn_validation_requested
            << " spawn_validation_sent=" << observation.spawn_validation_sent
            << " spawn_validation_failed=" << observation.spawn_validation_failed
            << " stage3_requested=" << observation.stage3_requested
            << " client_restart_sent=" << observation.client_restart_sent
            << " closed=" << observation.closed << " gameplay=0\n" << std::flush;
    }
private:
    std::ofstream file_;
    std::ostream* stream_ = nullptr;
};

bool mute_native_output() {
#ifdef _WIN32
    FILE* redirected = nullptr;
    if (freopen_s(&redirected, "NUL", "w", stdout) != 0
        || freopen_s(&redirected, "NUL", "w", stderr) != 0) return false;
    const HANDLE discard = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (discard == INVALID_HANDLE_VALUE) return false;
    // Retain this handle through native DLL process teardown as well.
    return SetStdHandle(STD_OUTPUT_HANDLE, discard) && SetStdHandle(STD_ERROR_HANDLE, discard);
#else
    return true; // The native Windows verifier is unavailable on this platform.
#endif
}

int serve_http(const Options& options, Diagnostics& diagnostics, hawken::auth::AuthService* auth,
               hawken::sync::SyncService* sync = nullptr,
               hawken::inventory::InventoryService* inventory = nullptr,
               hawken::sync::MenuSyncService* menu = nullptr,
               hawken::udp::UdpTrialRuntime* udp_trial = nullptr) {
    std::optional<hawken::master::MasterDirectory> directory;
    if (options.steam_local && auth && udp_trial && !options.udp_world_root.empty()
        && options.udp_trial && options.udp_trial->publication) {
        if (!udp_trial->running()) {
            std::osyncstream(diagnostics.stream()) << "Local UDP trial is not available.\n" << std::flush;
            return 1;
        }
        const auto text = [](const hawken::control::posthello::RawString& value) {
            if (value.form != hawken::control::posthello::StringForm::narrow
                || value.units.size() < 2 || value.units.size() > 257 || value.units.back() != 0)
                throw std::runtime_error("Invalid local trial listing metadata.");
            std::string result;
            result.reserve(value.units.size() - 1);
            for (std::size_t index = 0; index + 1 < value.units.size(); ++index) {
                const auto unit = value.units[index];
                if (unit < 0x20 || unit > 0x7e)
                    throw std::runtime_error("Invalid local trial listing metadata.");
                result.push_back(static_cast<char>(unit));
            }
            return result;
        };
        const auto& welcome = options.udp_trial->publication->welcome;
        // Explicit loopback Wreckage trial visibility, not a public directory
        // registration or proof of a playable world. The original browser uses
        // short mode HawkenDM; Welcome retains HawkenGameContent.HawkenDM.
        // Prematch describes this prepared publication and bound UDP trial, not gameplay proof.
        directory.emplace(std::vector<hawken::master::ServerMetadata>{{
            options.udp_trial->server_guid, options.udp_trial->region, hawken::udp::trial_game_version,
            "HawkLink Local Wreckage Trial", text(welcome.level), "HawkenDM",
            "127.0.0.1", udp_trial->port(), 1, true, false, hawken::master::MatchState::prematch}});
    }
    // Original PortTester checks TCP establishment, independently of game UDP.
    // Keep this loopback-only probe alive with HTTP/UDP, not with the client.
    std::unique_ptr<hawken::tcp::LocalTcpProbe> tcp_probe;
    if (udp_trial) {
        try { tcp_probe = std::make_unique<hawken::tcp::LocalTcpProbe>(15520); }
        catch (...) {
            std::osyncstream(diagnostics.stream()) << "Local TCP probe could not start.\n" << std::flush;
            return 1;
        }
    }
    // The immutable directory outlives the HTTP server and all its handlers.
    httplib::Server server;
    hawken::server::configure_server_http(server, auth, sync, inventory, menu,
        udp_trial ? &udp_trial->assignment() : nullptr, directory ? &*directory : nullptr,
        [&diagnostics](hawken::server::BrokerHttpRejectReason reason) {
            std::osyncstream(diagnostics.stream()) << "inventory_put reason="
                << (reason == hawken::server::BrokerHttpRejectReason::http_headers ? "http_headers" : "body_reader")
                << '\n' << std::flush;
        });
    const auto trial_listing = directory
        ? directory->respond("GET", "/gameServerListings").body.at("Result").at(0)
        : nlohmann::json{};
    server.set_logger([&diagnostics, trial_listing](const httplib::Request& request, const httplib::Response& response) {
        const auto diagnostic = hawken::http::classify_request(request.method, request.path);
        std::osyncstream(diagnostics.stream()) << "http status=" << response.status
            << " category=" << diagnostic.category << " method=" << diagnostic.method
            << '\n' << std::flush;
        if (request.method == "POST" && request.target == "/steam/login") {
            std::osyncstream(diagnostics.stream()) << "login claim_present="
                << (request.get_header_value_count("X-Hawken-SteamID") != 0) << '\n' << std::flush;
        }
        if (!trial_listing.is_null() && request.method == "GET"
            && request.path == "/gameServerListings" && response.status == 200) {
            const auto body = nlohmann::json::parse(response.body, nullptr, false);
            if (body.is_object() && body.contains("Result") && body.at("Result").is_array()) {
                std::osyncstream line(diagnostics.stream());
                line << "listing_count=" << body.at("Result").size();
                // Only fixed state labels are logged: no target, headers, query
                // values or response objects. Comparisons use httplib-decoded
                // params; they diagnose this trial, not native browser behavior.
                for (const auto* key : {"Region", "GameVersion", "Map", "GameType", "ServerName"}) {
                    const auto count = request.params.count(key);
                    const auto value = request.get_param_value(key);
                    const char* state = count == 0 ? "absent" : count != 1 ? "multiple"
                        : value.empty() ? "empty" : trial_listing.at(key) == value ? "match" : "mismatch";
                    line << ' ' << key << '=' << state;
                }
                line << '\n' << std::flush;
            }
        }
    });
    server.set_start_handler([&diagnostics, &options, auth, inventory, menu, udp_trial, &tcp_probe] {
        std::osyncstream(diagnostics.stream()) << "hawken-server http://127.0.0.1:" << options.port
            << " local_steam=" << (auth != nullptr) << " starter_inventory=" << (inventory != nullptr)
            << " menu_sync=" << (menu != nullptr)
            << " udp_bound=" << (udp_trial != nullptr && udp_trial->running())
            << " tcp_probe_bound=" << (tcp_probe != nullptr && tcp_probe->running())
            << " gameplay=0\n" << std::flush;
    });
    if (!server.bind_to_port("127.0.0.1", options.port)) {
        std::osyncstream(diagnostics.stream()) << "Local listener could not bind.\n" << std::flush;
        return 1;
    }

    try {
        std::atomic<bool> listener_finished{false};
        std::atomic<bool> trial_failed{false};
        std::jthread supervisor([&](std::stop_token stop) {
            std::optional<hawken::udp::LocalUdpDiagnostics> last_udp_state;
            while (!stop.stop_requested()
                && !listener_finished.load(std::memory_order_relaxed)) {
                if (udp_trial) {
                    const auto observation = udp_trial->diagnostics();
                    auto state = observation;
                    // Log protocol state changes while the client is connected.
                    // Traffic count alone must not produce one line per packet.
                    state.authenticated_datagrams = 0;
                    if (!last_udp_state || state != *last_udp_state) {
                        diagnostics.udp(observation);
                        last_udp_state = state;
                    }
                }
                std::error_code stop_error;
                const bool requested_by_file = !options.stop_file.empty()
                    && std::filesystem::exists(options.stop_file, stop_error);
                if ((udp_trial && !udp_trial->running())
                    || (tcp_probe && !tcp_probe->running()))
                    trial_failed.store(true, std::memory_order_relaxed);
                if ((shutdown_requested.load(std::memory_order_relaxed) || requested_by_file || stop_error
                    || trial_failed.load(std::memory_order_relaxed))
                    && server.is_running()) {
                    server.stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        });

        const bool listened = server.listen_after_bind();
        listener_finished.store(true, std::memory_order_relaxed);
        supervisor.request_stop();
        supervisor.join();
        server.stop();
        if (tcp_probe) tcp_probe->stop();
        if (!listened || trial_failed.load(std::memory_order_relaxed)) {
            std::osyncstream(diagnostics.stream()) << "Local listener could not run.\n" << std::flush;
            return 1;
        }
        std::osyncstream(diagnostics.stream()) << "Local backend listener stopped.\n" << std::flush;
        return 0;
    } catch (...) {
        server.stop();
        throw;
    }
}

int serve(const Options& options, const std::optional<StarterPolicy>& policy) {
    Diagnostics diagnostics(options.diagnostics);
    if (!options.steam_local) return serve_http(options, diagnostics, nullptr);
    if (!mute_native_output()) {
        std::osyncstream(diagnostics.stream()) << "Native output could not be contained.\n" << std::flush;
        return 1;
    }
    auto verifier = hawken::auth::make_local_steam_verifier(
        [&diagnostics](const auto& observation) { diagnostics.steam(observation); });
    if (!verifier) {
        std::osyncstream(diagnostics.stream()) << "Local Steam provider is not available.\n" << std::flush;
        return 1;
    }
    try {
        hawken::store::LocalStore database(options.database, options.server_id);
        hawken::auth::AuthService auth(database, *verifier);
        // Explicit first-local-server rule, not a recovered historical rate.
        // No currency is awarded by the read service.
        constexpr std::int32_t local_overflow_interval = 25000;
        hawken::sync::SyncService sync(database, auth, local_overflow_interval);
        hawken::sync::MenuSyncService menu(auth, database);
        std::unique_ptr<hawken::inventory::InventoryService> inventory;
        if (policy)
            inventory = std::make_unique<hawken::inventory::InventoryService>(database, auth,
                policy->ids, policy->balance, policy->offer_id,
                [&diagnostics](const hawken::inventory::PutObservation& observation) {
                    std::osyncstream line(diagnostics.stream());
                    line << "inventory_put reason="
                        << hawken::inventory::put_reason_name(observation.reason);
                    if (observation.reason == hawken::inventory::PutReason::reference_unknown_key) {
                        line << " reference_family="
                            << hawken::inventory::reference_family_name(observation.reference_family)
                            << " value_form="
                            << hawken::inventory::reference_value_form_name(observation.value_form);
                        if (observation.visor_key != hawken::inventory::VisorKey::none) {
                            line << " visor_key=" << static_cast<unsigned>(observation.visor_key)
                                << " visor_literal_present=" << (observation.visor_literal_size != 0);
                            if (observation.visor_literal_size != 0)
                                line << " visor_literal=" << std::string_view(
                                    observation.visor_literal.data(), observation.visor_literal_size);
                        }
                    }
                    line << '\n' << std::flush;
                });
        // Declared after Auth/Inventory: HTTP drains before UDP stops, and UDP
        // joins its callbacks before Assignment/Auth/SQL owners are destroyed.
        std::unique_ptr<hawken::udp::UdpTrialRuntime> udp_trial;
        if (options.udp_trial) {
            auto trial_config = *options.udp_trial;
            if (!options.udp_world_root.empty() && policy && inventory) {
                // Conditional original Link0/primary-map, all-members trial schema.
                // RPC export17404 -> field253/bound294; PRI MechSetup ->53/87.
                // See server-set-start-mech-20261014/contract-v3.json. This
                // activates an experiment, not native cache/possession proof.
                trial_config.start_mech = hawken::udp::StartMechDispatchPlan{
                    253,294,53,87,{true,true,true,{true,true,true},true,true,true},0};
                trial_config.start_mech_source = hawken::udp::StartMechSource{
                    policy->ids,[service=inventory.get()](const hawken::store::AccountRef& account,
                                                        std::string_view instance) {
                        return service->effective_owned_starter_mech(account,instance);
                    }};
                trial_config.spawn_validation = hawken::udp::SpawnValidationDispatchPlan{
                    249,250,251,294,{true,true,true,true,true,true}};
                trial_config.spawn_validation_source = hawken::udp::SpawnValidationSource{
                    [&database](const hawken::store::AccountRef& account) {
                        return database.account_achievement_ids(account);
                    },true};
                trial_config.spawn_admission = hawken::udp::SpawnAdmissionDispatchPlan{
                    281,83,294,{true,true,true,true,true,true}};
                // Conditional Link0 PRIMARY PRI cache41/87 and explicit Windows
                // little-endian archive trial. The observed UniqueId offset2d4
                // names the property; it does not prove this live field cache or
                // concrete native Archive. Identity comes only from authorized
                // SessionProvision.user_guid, never the client's Login.UniqueId.
                trial_config.pri_identity = hawken::udp::PriIdentityDispatchPlan{
                    41,87,hawken::control::Endian::little,{true,true,true,true,true}};
                // Version-pinned trial: actual R_GRI CDO GetPC-null/rotation-clear
                // prefix was observed passively. Class/RPC field IDs remain the
                // conditional PRIMARY all-members schema. These are LOCAL
                // package NetIndices, resolved against this connection's ranges.
                // GameClass and HUD are UClass references, not CDO references.
                trial_config.selection_bootstrap = hawken::udp::SelectionBootstrapPlan{
                    {{4,0,0,false},
                        {0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},
                        9439,{0,0,0},std::nullopt},
                    {0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU},105,
                    {0xbf5a6721U,0x4933092bU,0xe8522f80U,0x238fc144U},32596,
                    31,73,42,294,{true,true,true,true,true,true,true,true,true,true,true,true}};
                hawken::udp::ReadyMatchPlan ready{};
                ready.ready_rpc_field_id=280; ready.controller_exclusive_bound=294;
                ready.pri_ready_field_id=30; ready.pri_waiting_field_id=31; ready.pri_exclusive_bound=87;
                ready.gri_waiting_to_ready_field_id=48; ready.gri_waiting_for_field_id=49;
                ready.gri_countdown_field_id=51; ready.gri_match_begun_field_id=29; ready.gri_exclusive_bound=73;
                ready.integer_order=hawken::control::Endian::little;
                // Explicit one-player local policy. Maximum-delay override is
                // disabled (0), not a recovered concrete Hawken CDO value.
                ready.rules={true,1,0.55F,5,0,10,30.0F};
                ready.initial_state={0.0F,0.0F,false,0,1,-1};
                ready.role_is_player=true; ready.role_waiting_player=true;
                ready.world_tracking_created=true; ready.world_pending_quit=false;
                ready.eligibility={true,true,true,true,true,true,true,true,true,true,true,true,true};
                trial_config.ready_match=ready;
                trial_config.ready_match_source=hawken::udp::ReadyAccountSource{
                    [service=inventory.get()](const hawken::store::AccountRef& account)
                        ->std::optional<hawken::udp::ReadyAccountFacts> {
                        // A complete scoped SQL snapshot validates all four data
                        // domains without requiring selection or granting items.
                        const auto snapshot=service->owned_starter_account(account);
                        // These local SQL operations finish synchronously; no
                        // asynchronous purchase/save job is configured here.
                        return hawken::udp::ReadyAccountFacts{
                            snapshot.starter.has_value(),true,true,true,false,false};
                    }};
                // Our starter records are permanent; the current local domain
                // has no achievement awards. The reader verifies account scope
                // in SQL and must change before an award/import path is enabled.
                // RPC IDs still depend on the version-pinned trial class cache.
                std::osyncstream(diagnostics.stream())
                    << "udp_start_mech schema=conditional_primary_trial gameplay=0\n"
                    << "udp_spawn_validation schema=conditional_primary_trial gameplay=0\n"
                    << "udp_spawn_admission schema=conditional_primary_trial gameplay=0\n"
                    << "udp_pri_identity schema=conditional_primary_trial gameplay=0\n"
                    << "udp_selection_bootstrap schema=conditional_primary_trial gameplay=0\n"
                    << "udp_ready_match schema=conditional_primary_trial gameplay=0\n"
                    << std::flush;
            }
            udp_trial = std::make_unique<hawken::udp::UdpTrialRuntime>(auth, trial_config);
        }
        const int result = serve_http(options, diagnostics, &auth, &sync, inventory.get(), &menu,
            udp_trial.get());
        if (udp_trial) {
            udp_trial->stop();
            diagnostics.udp(udp_trial->diagnostics());
        }
        return result;
    } catch (...) {
        std::osyncstream(diagnostics.stream()) << "Local authenticated backend failed.\n" << std::flush;
        return 1;
    }
}
}

int main(int argc, char* argv[]) {
    Options options;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    try {
        if (!parse_options(argc, argv, options)) {
            std::cerr << "Invalid arguments. Use --help.\n";
            return 2;
        }
    } catch (...) {
        std::cerr << "Invalid arguments. Use --help.\n";
        return 2;
    }
    try {
        // Read-only pinned asset check before Steam, SQL, diagnostics or sockets.
        if (!options.udp_world_root.empty()) {
            hawken::game::configure_wreckage_trial(*options.udp_trial,options.udp_world_root);
            // The conditional native field schema is version-specific. Verify
            // the original executable before Steam, SQL or socket startup.
            if (!options.starter_profile.empty()
                && hawken::sha256_file(options.udp_world_root/"Binaries"/"Win64"/
                    "HawkenGame-Win64-Shipping.exe")
                    != "4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2")
                throw std::runtime_error("invalid native trial schema");
        }
    } catch (...) {
        std::cerr << "Invalid Wreckage trial assets.\n";
        return 2;
    }
    std::optional<StarterPolicy> policy;
    try {
        if (!options.starter_profile.empty()) policy = read_starter_policy(options.starter_profile);
    } catch (...) {
        std::cerr << "Invalid starter profile.\n";
        return 2;
    }
    if (std::signal(SIGINT, request_shutdown) == SIG_ERR
        || std::signal(SIGTERM, request_shutdown) == SIG_ERR) {
        std::cerr << "Shutdown signals could not be configured.\n";
        return 1;
    }
#if defined(_WIN32) && defined(SIGBREAK)
    if (std::signal(SIGBREAK, request_shutdown) == SIG_ERR) {
        std::cerr << "Shutdown signals could not be configured.\n";
        return 1;
    }
#endif
    try {
        return serve(options, policy);
    } catch (...) {
        std::cerr << "Local backend listener failed.\n";
        return 1;
    }
}
