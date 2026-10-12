#include "hawken/session_assignment.hpp"
#include "hawken/meteor_compat.hpp"
#include <array>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
using hawken::auth::AuthReply;
using Json = nlohmann::json;
constexpr std::string_view ad_path = "/hawkenClientMatchmakingAdvertisements";
constexpr std::string_view ad_prefix = "/hawkenClientMatchmakingAdvertisements/";
enum class Route { unknown, session, create_ad, read_ad };
struct Target { Route route = Route::unknown; std::string_view guid; };

bool guid_valid(std::string_view guid) noexcept {
    if (guid.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < guid.size(); ++i) {
        const char c = guid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            if (c != '0') nonzero = true;
        }
    }
    return nonzero;
}
bool token_valid(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64) return false;
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}
bool loopback_valid(std::string_view ip) noexcept {
    std::array<int, 4> parts{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto end = ip.find('.', start);
        if (end == std::string_view::npos) {
            if (i != 3) return false;
            end = ip.size();
        } else if (i == 3) { return false; }
        const auto part = ip.substr(start, end - start);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0')) return false;
        for (const char c : part) {
            if (c < '0' || c > '9') return false;
            parts[i] = parts[i] * 10 + (c - '0');
        }
        if (parts[i] > 255) return false;
        start = end + 1;
    }
    return parts[0] == 127;
}
Target parse_target(std::string_view raw) noexcept {
    if (raw == "/session") return {Route::session, {}};
    if (raw == ad_path) return {Route::create_ad, {}};
    if (raw.starts_with(ad_prefix)) {
        const auto guid = raw.substr(ad_prefix.size());
        if (guid_valid(guid)) return {Route::read_ad, guid};
    }
    return {};
}
AuthReply failure(int status) {
    switch (status) {
    case 400: return {400, "Invalid session assignment request.", nullptr};
    case 401: return {401, "Authentication required.", nullptr};
    case 403: return {403, "Access was rejected.", nullptr};
    case 404: return {404, "Resource not found.", nullptr};
    case 405: return {405, "Method not allowed.", nullptr};
    case 503: return {503, "Session assignment is not available.", nullptr};
    default: return {500, "Request could not be processed.", nullptr};
    }
}
void erase(std::span<std::uint8_t> bytes) noexcept {
    volatile std::uint8_t* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
}
struct PrivateString {
    std::string text;
    ~PrivateString() {
        erase({reinterpret_cast<std::uint8_t*>(text.data()), text.size()});
    }
};
std::string base64url(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((bytes.size() * 4 + 2) / 3);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        auto block = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) block |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) block |= bytes[i + 2];
        output += alphabet[(block >> 18) & 63U];
        output += alphabet[(block >> 12) & 63U];
        if (i + 1 < bytes.size()) output += alphabet[(block >> 6) & 63U];
        if (i + 2 < bytes.size()) output += alphabet[block & 63U];
    }
    return output;
}
std::string uuid_text(const std::array<std::uint8_t, 16>& bytes) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string guid;
    guid.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) guid += '-';
        guid += hex[bytes[i] >> 4];
        guid += hex[bytes[i] & 15];
    }
    return guid;
}
std::string utc_text(std::int64_t seconds) {
    if (seconds < 0 || seconds > 253402300799LL) throw std::runtime_error("Invalid clock.");
    const std::chrono::sys_seconds time{std::chrono::seconds{seconds}};
    const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(time)};
    const int year = static_cast<int>(date.year());
    std::string result = "0000-00-00T00:00:00Z";
    for (int i = 0, divisor = 1000; i < 4; ++i, divisor /= 10)
        result[static_cast<std::size_t>(i)] = static_cast<char>('0' + (year / divisor) % 10);
    const auto pair = [&](std::size_t offset, int value) {
        result[offset] = static_cast<char>('0' + value / 10);
        result[offset + 1] = static_cast<char>('0' + value % 10);
    };
    pair(5, static_cast<int>(static_cast<unsigned>(date.month())));
    pair(8, static_cast<int>(static_cast<unsigned>(date.day())));
    const auto within_day = seconds % 86400;
    pair(11, static_cast<int>(within_day / 3600));
    pair(14, static_cast<int>((within_day % 3600) / 60));
    pair(17, static_cast<int>(within_day % 60));
    return result;
}
struct Post {
    std::string owner;
    bool requested = false;
};
struct InvalidBody {};
std::optional<Post> parse_post(std::string_view body, const hawken::assignment::Destination& destination) {
    if (body.empty() || body.size() > hawken::assignment::SessionAssignmentService::max_request_bytes)
        return std::nullopt;
    constexpr std::array<std::string_view, 5> names{
        "OwnerGuid", "Region", "GameVersion", "Users", "RequestedServerGuid"};
    std::array<bool, 5> seen{};
    Json parsed;
    try {
        parsed = Json::parse(body.begin(), body.end(),
            [&](int depth, Json::parse_event_t event, Json& value) {
                // The valid shape has only one root object and Users array.
                // Reject nested containers early rather than retaining them.
                if ((event == Json::parse_event_t::object_start && depth != 0)
                    || (event == Json::parse_event_t::array_start && depth != 1)) throw InvalidBody{};
                if (event == Json::parse_event_t::key) {
                    const auto& key = value.get_ref<const std::string&>();
                    std::size_t i = 0;
                    while (i < names.size() && names[i] != key) ++i;
                    if (i == names.size() || seen[i]) throw InvalidBody{};
                    seen[i] = true;
                }
                return true;
            }, false);
    } catch (const InvalidBody&) { return std::nullopt; }
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    for (std::size_t i = 0; i < 4; ++i) if (!seen[i]) return std::nullopt;
    if (!parsed["OwnerGuid"].is_string() || !parsed["Region"].is_string()
        || !parsed["GameVersion"].is_string() || !parsed["Users"].is_array()) return std::nullopt;
    Post post{parsed["OwnerGuid"].get<std::string>(), seen[4]};
    const auto& users = parsed["Users"];
    if (!guid_valid(post.owner) || parsed["Region"] != destination.region
        || parsed["GameVersion"] != destination.game_version || users.size() != 1
        || !users[0].is_string() || users[0] != post.owner) return std::nullopt;
    if (post.requested && (!parsed["RequestedServerGuid"].is_string()
        || parsed["RequestedServerGuid"] != destination.server_guid)) return std::nullopt;
    return post;
}
}

namespace hawken::assignment {
struct SessionAssignmentService::Impl {
    struct Advertisement { std::string guid; std::string time_created; bool requested = false; };
    struct State {
        SessionProvision provision;
        std::string authorization;
        std::optional<Advertisement> advertisement;
        std::unique_ptr<UdpSessionLease> lease;
        ~State() {
            // Retire the owner Context before erasing our material. The owner
            // synchronizes in-flight UDP users without reentering this service.
            lease.reset();
            erase(provision.id);
            erase(provision.key);
            erase({reinterpret_cast<std::uint8_t*>(authorization.data()), authorization.size()});
        }
    };
    auth::AuthService& auth;
    Destination destination;
    AssignmentPolicy policy;
    Clock monotonic_clock;
    Clock utc_clock;
    RandomFill random;
    DestinationBound bound;
    Handoff handoff;
    std::mutex mutex;
    std::optional<std::int64_t> last_monotonic;
    std::unique_ptr<State> current; // Fixed capacity one; no accumulating map.
    Impl(auth::AuthService& service, Destination target, AssignmentPolicy rules,
         Clock monotonic, Clock utc, RandomFill entropy,
         DestinationBound binding, Handoff install)
        : auth(service), destination(std::move(target)), policy(rules),
          monotonic_clock(std::move(monotonic)), utc_clock(std::move(utc)),
          random(std::move(entropy)), bound(std::move(binding)), handoff(std::move(install)) {
        if (!guid_valid(destination.server_guid) || !loopback_valid(destination.ip)
            || destination.port < 1 || destination.port > 65535
            || !token_valid(destination.region) || !token_valid(destination.game_version)
            || policy.lifetime_seconds < 1 || policy.lifetime_seconds > 900
            || !monotonic_clock || !utc_clock || !random || !bound || !handoff)
            throw std::invalid_argument("Invalid session assignment configuration.");
    }
    auth::AccessReply access(std::string_view authorization,
        std::optional<std::string_view> guid = std::nullopt) {
        auto incoming = auth.access(authorization, guid);
        if (current) {
            // A foreign request must not evict a valid retained grant. Recheck
            // that grant separately when the incoming snapshot differs/fails.
            const bool same = incoming.status == 200 && incoming.account
                && authorization == current->authorization
                && incoming.account->account == current->provision.account
                && incoming.account->user_guid == current->provision.user_guid;
            if (!same) {
                const auto retained = auth.access(current->authorization, current->provision.user_guid);
                if (retained.status != 200 || !retained.account
                    || retained.account->account != current->provision.account
                    || retained.account->user_guid != current->provision.user_guid) current.reset();
            }
        }
        if (incoming.status == 200 && !incoming.account) return {500, std::nullopt};
        return incoming;
    }
    std::optional<std::int64_t> time_now() {
        const auto now = monotonic_clock();
        if (now < 0 || (last_monotonic && now < *last_monotonic)) {
            current.reset();
            return std::nullopt;
        }
        last_monotonic = now;
        return now;
    }
    int maintain(std::int64_t now) {
        if (!bound(destination)) { current.reset(); return 503; }
        if (current && now >= current->provision.expires_at_monotonic) current.reset();
        if (current && !current->lease->active()) { current.reset(); return 503; }
        return 200;
    }
    std::optional<Advertisement> make_advertisement(bool requested) {
        std::array<std::uint8_t, 16> bytes{};
        if (!random(bytes)) return std::nullopt;
        auto guid = uuid_text(bytes);
        if (!guid_valid(guid)) return std::nullopt;
        return Advertisement{std::move(guid), utc_text(utc_clock()), requested};
    }
    int confirm(State& candidate, std::int64_t now) {
        if (!candidate.lease || !candidate.lease->active()) return 503;
        const auto checked = auth.access(candidate.authorization, candidate.provision.user_guid);
        if (checked.status != 200) return checked.status;
        if (!checked.account || checked.account->account != candidate.provision.account) return 500;
        const auto after = time_now();
        if (!after || *after < now) return 500;
        if (*after >= candidate.provision.expires_at_monotonic) return 503;
        if (!bound(destination) || !candidate.lease->active()) return 503;
        return 200;
    }
    std::unique_ptr<State> prepare(const auth::AuthenticatedAccount& account,
        std::string_view authorization, std::int64_t now) {
        if (now > std::numeric_limits<std::int64_t>::max() - policy.lifetime_seconds) return nullptr;
        auto candidate = std::make_unique<State>();
        candidate->provision.account = account.account;
        candidate->provision.user_guid = account.user_guid;
        candidate->provision.destination = destination;
        candidate->provision.expires_at_monotonic = now + policy.lifetime_seconds;
        candidate->authorization = authorization;
        if (!random(candidate->provision.id) || !random(candidate->provision.key)) return nullptr;
        return candidate;
    }
    AuthReply select(std::string_view authorization, std::string_view server_guid) {
        if (!guid_valid(server_guid)) return failure(400);
        const auto authorized = access(authorization);
        if (authorized.status != 200) return failure(authorized.status);
        if (server_guid != destination.server_guid) return failure(404);
        const auto now = time_now();
        if (!now) return failure(500);
        const int maintained = maintain(*now);
        if (maintained != 200) return failure(maintained);
        if (current) return {200, "", nullptr};
        auto candidate = prepare(*authorized.account, authorization, *now);
        if (!candidate) return failure(500);
        candidate->lease = handoff(candidate->provision);
        const int confirmed = confirm(*candidate, *now);
        if (confirmed != 200) return failure(confirmed);
        current = std::move(candidate);
        return {200, "", nullptr};
    }
    AuthReply create_ad(const Post& post, const auth::AuthenticatedAccount& account,
        std::string_view authorization, std::int64_t now) {
        if (current && current->advertisement)
            return {200, "", current->advertisement->guid};
        if (!current && !post.requested) return failure(404);
        auto candidate = current ? nullptr : prepare(account, authorization, now);
        if (!current && !candidate) return failure(500);
        auto advertisement = make_advertisement(post.requested);
        if (!advertisement) return failure(500);
        // Allocate the outward reply before installing a new Context. Commit
        // uses moves only, so a failure cannot publish a partial advertisement.
        AuthReply reply{200, "", advertisement->guid};
        if (candidate) {
            candidate->advertisement.emplace(std::move(*advertisement));
            candidate->lease = handoff(candidate->provision);
            const int confirmed = confirm(*candidate, now);
            if (confirmed != 200) return failure(confirmed);
            current = std::move(candidate);
        } else {
            const int confirmed = confirm(*current, now);
            if (confirmed != 200) { current.reset(); return failure(confirmed); }
            current->advertisement.emplace(std::move(*advertisement));
        }
        return reply;
    }
    AuthReply session_reply() const {
        PrivateString id{base64url(current->provision.id)};
        PrivateString key{base64url(current->provision.key)};
        meteor::ClientSession dto{current->provision.user_guid, id.text, key.text,
            destination.ip, destination.server_guid,
            current->advertisement ? current->advertisement->guid : std::string{}, destination.port};
        auto result = meteor::session_json(dto);
        erase({reinterpret_cast<std::uint8_t*>(dto.session_id.data()), dto.session_id.size()});
        erase({reinterpret_cast<std::uint8_t*>(dto.session_key.data()), dto.session_key.size()});
        return {200, "", std::move(result)};
    }
    AuthReply advertisement_reply() const {
        const auto& ad = *current->advertisement;
        Json result{{"Guid", ad.guid}, {"TimeCreated", ad.time_created},
            {"OwnerGuid", current->provision.user_guid}, {"Region", destination.region},
            {"GameVersion", destination.game_version}, {"Users", Json::array({current->provision.user_guid})},
            {"Status", 1}, {"SkillRatingAverage", policy.skill_rating_average},
            {"SkillRatingSearchMin", policy.skill_rating_search_min},
            {"AssignedServerGuid", destination.server_guid}, {"AssignedServerIp", destination.ip},
            {"AssignedServerPort", destination.port}};
        if (ad.requested) result["RequestedServerGuid"] = destination.server_guid;
        return {200, "", std::move(result)};
    }
    AuthReply handle(const Target& target, std::string_view authorization, std::string_view body) {
        std::optional<Post> post;
        if (target.route == Route::create_ad) {
            post = parse_post(body, destination);
            if (!post) return failure(400);
        }
        const auto authorized = access(authorization,
            post ? std::optional<std::string_view>{post->owner} : std::nullopt);
        if (authorized.status != 200) return failure(authorized.status);
        const auto now = time_now();
        if (!now) return failure(500);
        const int maintained = maintain(*now);
        if (maintained != 200) return failure(maintained);
        if (post) return create_ad(*post, *authorized.account, authorization, *now);
        if (!current) return failure(404);
        if (target.route == Route::session) return session_reply();
        if (!current->advertisement || target.guid != current->advertisement->guid) return failure(404);
        return advertisement_reply();
    }
    bool authorize_udp(const session::SessionId& id) {
        if (!current) return false;
        const auto authorized = access(current->authorization, current->provision.user_guid);
        if (authorized.status != 200 || !current) return false;
        const auto now = time_now();
        if (!now || maintain(*now) != 200 || !current) return false;
        return current->provision.id == id;
    }
};
SessionAssignmentService::SessionAssignmentService(auth::AuthService& auth,
    Destination destination, AssignmentPolicy policy, Clock monotonic_clock,
    Clock utc_clock, RandomFill random, DestinationBound bound, Handoff handoff)
    : impl_(std::make_unique<Impl>(auth, std::move(destination), policy,
          std::move(monotonic_clock), std::move(utc_clock), std::move(random),
          std::move(bound), std::move(handoff))) {}
SessionAssignmentService::~SessionAssignmentService() = default;
bool SessionAssignmentService::supports_target(std::string_view raw_target) const {
    return parse_target(raw_target).route != Route::unknown;
}
auth::AuthReply SessionAssignmentService::select(std::string_view authorization, std::string_view server_guid) {
    try {
        std::lock_guard lock(impl_->mutex);
        try { return impl_->select(authorization, server_guid); }
        catch (...) { impl_->current.reset(); return failure(500); }
    } catch (...) { return failure(500); }
}
auth::AuthReply SessionAssignmentService::handle(std::string_view method, std::string_view raw_target,
    std::string_view authorization, std::string_view body) {
    const auto target = parse_target(raw_target);
    if (target.route == Route::unknown) return failure(404);
    const auto expected_method = target.route == Route::create_ad ? "POST" : "GET";
    if (method != expected_method) return failure(405);
    if (target.route != Route::create_ad && !body.empty()) return failure(400);
    try {
        std::lock_guard lock(impl_->mutex);
        try { return impl_->handle(target, authorization, body); }
        catch (...) { impl_->current.reset(); return failure(500); }
    } catch (...) { return failure(500); }
}
bool SessionAssignmentService::validate_login_response(const session::SessionId& id,
    const control::posthello::RawString& response) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        try {
            // The internal check can retire current. Do not retain provision
            // references across it or call the public locking wrapper here.
            if (!impl_->authorize_udp(id) || !impl_->current) return false;
            using Form = control::posthello::StringForm;
            if (response.form != Form::narrow && response.form != Form::wide) return false;
            if (response.units.size() != 36 && response.units.size() != 37) return false;
            if (response.units.size() == 37 && response.units[36] != 0) return false;
            std::array<char, 36> normalized{};
            for (std::size_t i = 0; i < normalized.size(); ++i) {
                const auto unit = response.units[i];
                if (unit > 0x7f) return false;
                char value = static_cast<char>(unit);
                if (i == 8 || i == 13 || i == 18 || i == 23) {
                    if (value != '-') return false;
                } else {
                    if (value >= 'A' && value <= 'F') value = static_cast<char>(value - 'A' + 'a');
                    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
                        return false;
                }
                normalized[i] = value;
            }
            return std::string_view(normalized.data(), normalized.size()) == impl_->current->provision.user_guid;
        } catch (...) { impl_->current.reset(); return false; }
    } catch (...) { return false; }
}
bool SessionAssignmentService::authorize_udp(const session::SessionId& id) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        try { return impl_->authorize_udp(id); }
        catch (...) { impl_->current.reset(); return false; }
    } catch (...) { return false; }
}
}
