#include "hawken/server_http.hpp"
#include "hawken/http_diagnostics.hpp"
#include "hawken/auth_service.hpp"
#include "hawken/sync_service.hpp"
#include "hawken/inventory_service.hpp"
#include "hawken/menu_sync_service.hpp"
#include "hawken/steam_local_auth.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include "hawken/wreckage_trial.hpp"
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
        }
        std::osyncstream(*stream_) << "steam phase=" << phase
            << " available=" << observation.available << " begin_ok=" << observation.begin_ok
            << " packed20=" << observation.packed20 << " identity_match=" << observation.identity_match
            << " response_ok=" << observation.response_ok << " license_ok=" << observation.license_ok
            << " gs_shutdown=" << observation.gs_shutdown_returned
            << " gs_pipe_zero=" << observation.gs_pipe_zero << '\n' << std::flush;
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
        // registration or proof of a playable world. Keep original identifiers;
        // no inferred browser GameType enum or map alias.
        directory.emplace(std::vector<hawken::master::ServerMetadata>{{
            options.udp_trial->server_guid, options.udp_trial->region, "1.6.0.154767",
            "HawkLink Local Wreckage Trial", text(welcome.level), text(welcome.game),
            "127.0.0.1", udp_trial->port(), 1, true, false}});
    }
    // The immutable directory outlives the HTTP server and all its handlers.
    httplib::Server server;
    hawken::server::configure_server_http(server, auth, sync, inventory, menu,
        udp_trial ? &udp_trial->assignment() : nullptr, directory ? &*directory : nullptr);
    const auto trial_listing = directory
        ? directory->respond("GET", "/gameServerListings").body.at("Result").at(0)
        : nlohmann::json{};
    server.set_logger([&diagnostics, trial_listing](const httplib::Request& request, const httplib::Response& response) {
        const auto diagnostic = hawken::http::classify_request(request.method, request.path);
        std::osyncstream(diagnostics.stream()) << "http status=" << response.status
            << " category=" << diagnostic.category << " method=" << diagnostic.method
            << '\n' << std::flush;
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
    server.set_start_handler([&diagnostics, &options, auth, inventory, menu, udp_trial] {
        std::osyncstream(diagnostics.stream()) << "hawken-server http://127.0.0.1:" << options.port
            << " local_steam=" << (auth != nullptr) << " starter_inventory=" << (inventory != nullptr)
            << " menu_sync=" << (menu != nullptr)
            << " udp_bound=" << (udp_trial != nullptr && udp_trial->running())
            << " gameplay=0\n" << std::flush;
    });
    if (!server.bind_to_port("127.0.0.1", options.port)) {
        std::osyncstream(diagnostics.stream()) << "Local listener could not bind.\n" << std::flush;
        return 1;
    }

    try {
        std::atomic<bool> listener_finished{false};
        std::atomic<bool> udp_failed{false};
        std::jthread supervisor([&](std::stop_token stop) {
            while (!stop.stop_requested()
                && !listener_finished.load(std::memory_order_relaxed)) {
                std::error_code stop_error;
                const bool requested_by_file = !options.stop_file.empty()
                    && std::filesystem::exists(options.stop_file, stop_error);
                if (udp_trial && !udp_trial->running())
                    udp_failed.store(true, std::memory_order_relaxed);
                if ((shutdown_requested.load(std::memory_order_relaxed) || requested_by_file || stop_error
                    || udp_failed.load(std::memory_order_relaxed))
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
        if (!listened || udp_failed.load(std::memory_order_relaxed)) {
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
                policy->ids, policy->balance, policy->offer_id);
        // Declared after Auth/Inventory: HTTP drains before UDP stops, and UDP
        // joins its callbacks before Assignment/Auth/SQL owners are destroyed.
        std::unique_ptr<hawken::udp::UdpTrialRuntime> udp_trial;
        if (options.udp_trial)
            udp_trial = std::make_unique<hawken::udp::UdpTrialRuntime>(auth, *options.udp_trial);
        const int result = serve_http(options, diagnostics, &auth, &sync, inventory.get(), &menu,
            udp_trial.get());
        if (udp_trial) {
            udp_trial->stop();
            const auto observation = udp_trial->diagnostics();
            std::osyncstream(diagnostics.stream())
                << "udp authenticated_datagrams=" << observation.authenticated_datagrams
                << " hello_validated=" << observation.hello_validated
                << " login_received=" << observation.login_received
                << " login_response_validated=" << observation.login_response_validated
                << " world_published=" << observation.world_published
                << " join_requested=" << observation.join_requested
                << " controller_sent=" << observation.controller_sent
                << " pawn_sent=" << observation.pawn_sent
                << " closed=" << observation.closed << " gameplay=0\n" << std::flush;
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
            options.udp_trial->publication = hawken::game::load_wreckage_trial(options.udp_world_root);
            // Explicit local Wreckage trial policy, not historical spawn selection
            // or collision proof. Observed controller CDO bit0 is false: no rotation.
            options.udp_trial->controller = hawken::udp::InitialControllerPlan{
                {1, 0, 0, false},
                {0xbf5a6721U, 0x4933092bU, 0xe8522f80U, 0x238fc144U},
                18053, {-1472, 2432, 193}, std::nullopt, 0};
            // Explicit local Pawn trial policy. Loaded R_PawnContent CDO has
            // NetIndex233/rotation bit0 clear; no collision/possession proof.
            options.udp_trial->pawn = hawken::udp::InitialPawnPlan{
                {2, 0, 0, false},
                {0x5cd139c7U, 0x4aa73e14U, 0xbd1c8593U, 0xf86056ddU},
                233, {-1472, 2432, 193}, std::nullopt};
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
