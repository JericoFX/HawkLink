#include "hawken/server_http.hpp"
#include "hawken/master_directory.hpp"
#include "hawken/session_assignment.hpp"
#include "hawken/udp_trial_runtime.hpp"
#include "test_support.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace hawken;
using Json = nlohmann::json;
constexpr std::string_view local_guid = "12345678-1234-5678-9012-123456789abc";
constexpr std::string_view foreign_guid = "98765432-1234-5678-9012-123456789abc";
const std::string individual = "/gameServerListings/" + std::string(local_guid);
class Verifier final : public auth::SteamVerifier {
public:
    std::atomic<bool> active{false};
    std::atomic<int> calls{0};
    auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        ++calls;
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? auth::SteamAuthStatus::authenticated : auth::SteamAuthStatus::invalid_ticket,
                active ? 76561197960265729ULL : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == 76561197960265729ULL;
    }
    void end_session(std::uint64_t) override { active = false; }
};
class Lease final : public assignment::UdpSessionLease {
public:
    explicit Lease(const assignment::SessionProvision& provision)
        : context(session::Role::server, provision.id, provision.key) {}
    bool active() const noexcept override { return true; }
private:
    session::Context context;
};
class Listener {
public:
    test::TempDirectory temporary;
    store::LocalStore store{temporary.path() / "accounts.sqlite", "listing-http"};
    Verifier verifier;
    std::atomic<std::int64_t> monotonic{100};
    std::atomic<bool> bound{true};
    std::atomic<int> installs{0};
    auth::AuthService auth{store, verifier, [] { return 1000LL; }};
    assignment::SessionAssignmentService assignment{
        auth, {std::string(local_guid), "127.0.0.1", 27015, "EU", "1.6.0.154767"},
        {30,1200,800}, [this] { return monotonic.load(); }, [] { return 1704067200LL; },
        [](std::span<std::uint8_t> bytes) {
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(bytes.size() == 32 ? i : i * 17);
            return true;
        }, [this](const assignment::Destination&) { return bound.load(); },
        [this](const assignment::SessionProvision& provision) {
            ++installs; return std::make_unique<Lease>(provision);
        }};
    // Public synthetic metadata; neither map names nor these fixtures prove a world.
    master::MasterDirectory directory{{
        {std::string(local_guid),"EU","1.6.0.154767","Local","Fixture","FixtureMode",
            "127.0.0.1",27015,1,true,false},
        {std::string(foreign_guid),"NA","1.6.0.154767","Foreign","Fixture","FixtureMode",
            "127.0.0.2",27016,1,true,false}}};
    explicit Listener(bool directory_enabled = true, bool assignment_enabled = true) {
        server::configure_server_http(server_, &auth, nullptr, nullptr, nullptr,
            assignment_enabled ? &assignment : nullptr, directory_enabled ? &directory : nullptr);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Public listing fixture could not bind.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) throw std::runtime_error("Public listing fixture did not start.");
    }
    ~Listener() { server_.stop(); }
    int port() const { return port_; }
private:
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};
class TrialListener {
public:
    test::TempDirectory temporary;
    store::LocalStore store{temporary.path() / "accounts.sqlite", "trial-listing-http"};
    Verifier verifier;
    auth::AuthService auth{store, verifier};
    udp::UdpTrialConfig config;
    udp::UdpTrialRuntime runtime;
    std::optional<master::MasterDirectory> directory;
    explicit TrialListener(bool world = true) : config(make_config(world)), runtime(auth, config) {
        if (config.publication) {
            // Synthetic caller plan; no original assets or gameplay are claimed.
            directory.emplace(std::vector<master::ServerMetadata>{{
                config.server_guid,config.region,"547429238","HawkLink Local Wreckage Trial",
                "VS-Wreckage","HawkenDM","127.0.0.1",runtime.port(),1,true,false,
                master::MatchState::prematch}});
        }
        server::configure_server_http(server_, &auth, nullptr, nullptr, nullptr,
            &runtime.assignment(), directory ? &*directory : nullptr);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Local trial HTTP fixture could not bind.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) throw std::runtime_error("Local trial HTTP fixture did not start.");
    }
    ~TrialListener() {
        server_.stop();
        if (worker_.joinable()) worker_.join();
        runtime.stop();
    }
    int port() const { return port_; }
private:
    static udp::UdpTrialConfig make_config(bool world) {
        udp::UdpTrialConfig result{0,std::string(local_guid),"EU"};
        if (world) {
            game::WorldPublication plan;
            plan.packages={{"Fixture",{1,2,3,4},{8}}};
            using control::posthello::StringForm;
            plan.welcome={{StringForm::narrow,std::u16string(u"VS-Wreckage",12)},
                          {StringForm::narrow,std::u16string(u"HawkenGameContent.HawkenDM",27)}};
            result.publication=std::move(plan);
        }
        return result;
    }
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};
Json result(const httplib::Result& response, int status) {
    REQUIRE(response); REQUIRE(response->status == status);
    CHECK(response->get_header_value("Connection") == "close");
    CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    const auto body = Json::parse(response->body);
    CHECK(body.at("Status") == status); CHECK(body.at("Message").is_string());
    return body.at("Result");
}
Json login(httplib::Client& client) {
    return result(client.Post("/steam/login", R"({"SteamAuthTicket":"AABB0100"})", "application/json"),200);
}
httplib::Headers headers(const Json& credentials) {
    return {{"Authorization", "Basic " + credentials.at("AccessGrant").get<std::string>()}};
}
}

TEST_CASE("Listing collection exposes public directory metadata without granting a session") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto all = result(client.Get("/gameServerListings"),200);
    REQUIRE(all.is_array()); REQUIRE(all.size()==2);
    CHECK(all[0].at("Guid")==std::string(local_guid)); CHECK(all[0].at("Port")=="27015");
    CHECK(all[0].at("Users")==Json::array());
    CHECK(all[0].at("DeveloperData")==Json{{"MatchState","0"}});
    CHECK_FALSE(all[0].contains("SessionKey")); CHECK_FALSE(all[0].contains("AccessGrant"));
    const auto filtered = result(client.Get("/gameServerListings?Region=NA&GameVersion=1.6.0.154767"),200);
    REQUIRE(filtered.size()==1); CHECK(filtered[0].at("Guid")==std::string(foreign_guid));
    CHECK(listener.installs==0); CHECK(listener.verifier.calls==0);
    CHECK(result(client.Get("/session"),401).is_null());
}
TEST_CASE("Listing individual selection requires server authorization and reuses one provision") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    CHECK(result(client.Get(individual),401).is_null()); CHECK(listener.installs==0);
    const auto credentials = login(client); const auto authorization = headers(credentials);
    CHECK(result(client.Get("/session",authorization),404).is_null());
    const auto listing = result(client.Get(individual,authorization),200);
    CHECK(listing==listener.directory.respond("GET",individual).body.at("Result"));
    CHECK(listing.at("DeveloperData")==Json{{"MatchState","0"}});
    const auto session = result(client.Get("/session",authorization),200);
    CHECK(session.at("AssignedServerGuid")==std::string(local_guid));
    CHECK(session.at("ClientAdvertisementGuid")=="");
    CHECK(session.at("UserGuid")==credentials.at("UserId"));
    CHECK(session.at("SessionId")=="ABEiM0RVZnc");
    CHECK(session.at("SessionKey")=="AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
    CHECK(result(client.Get(individual,authorization),200)==listing);
    CHECK(result(client.Get("/session",authorization),200)==session);
    CHECK(listener.installs==1); CHECK(listener.verifier.calls==1);
}
TEST_CASE("Listing individual selection cannot assign another server or stale authorization") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials = login(client); const auto authorization = headers(credentials);
    CHECK(result(client.Get("/gameServerListings/"+std::string(foreign_guid),authorization),404).is_null());
    CHECK(listener.installs==0);
    listener.bound=false; CHECK(result(client.Get(individual,authorization),503).is_null());
    CHECK(listener.installs==0); listener.bound=true;
    REQUIRE(result(client.Get(individual,authorization),200).is_object());
    listener.verifier.active=false;
    CHECK(result(client.Get(individual,authorization),401).is_null());
    CHECK(result(client.Get("/session",authorization),401).is_null());
}
TEST_CASE("Listing facade reads complete GET bodies and preserves directory target validation") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    client.set_path_encode(false);
    const auto method = client.Post(individual,"{}","application/json");
    CHECK(result(method,405).is_null()); CHECK(method->get_header_value("Allow")=="GET");
    CHECK(result(client.Get(individual+"?Region=EU"),400).is_null());
    CHECK(result(client.Get("/gameServerListings?Region=EU&Region=NA"),400).is_null());
    CHECK(result(client.Get("/gameServerListings/../"),400).is_null());
    CHECK(result(client.Get("/gameServerListingsElse"),404).is_null());
    const auto credentials = login(client); const auto authorization = headers(credentials);
    for (const auto& path : {individual,std::string("/gameServerListings")}) {
        httplib::Request request;
        request.method="GET"; request.path=path; request.headers=authorization; request.body="{}";
        CHECK(result(client.send(request),400).is_null());
    }
    CHECK(listener.installs==0);
}
TEST_CASE("Listing optional snapshot and assignment preserve their separate responsibilities") {
    Listener absent(false); httplib::Client original("127.0.0.1",absent.port());
    CHECK(result(original.Get("/gameServerListings"),404).is_null());
    CHECK(result(original.Get("/version"),200).is_object());
    Listener directory_only(true,false); httplib::Client read_only("127.0.0.1",directory_only.port());
    CHECK(result(read_only.Get(individual),200).is_object());
    const auto credentials = login(read_only);
    CHECK(result(read_only.Get("/session",headers(credentials)),404).is_null());
    CHECK(directory_only.installs==0);
}

TEST_CASE("Local trial listing selects the actual bound UDP destination and preserves its session") {
    TrialListener listener; httplib::Client client("127.0.0.1",listener.port());
    REQUIRE(listener.runtime.running()); REQUIRE(listener.runtime.port()!=0);
    const auto all=result(client.Get("/gameServerListings"),200);
    REQUIRE(all.is_array()); REQUIRE(all.size()==1);
    CHECK(all[0].at("Guid")==std::string(local_guid));
    CHECK(all[0].at("Endpoint")=="127.0.0.1");
    CHECK(all[0].at("Port")==std::to_string(listener.runtime.port()));
    CHECK(all[0].at("ServerName")=="HawkLink Local Wreckage Trial");
    CHECK(all[0].at("Map")=="VS-Wreckage");
    CHECK(all[0].at("GameType")=="HawkenDM");
    CHECK(all[0].at("Region")=="EU");
    CHECK(all[0].at("GameVersion")=="547429238");
    CHECK(all[0].at("MaxUsers")==1);
    CHECK(all[0].at("IsPublicVisible")==true);
    CHECK(all[0].at("IsMatchmakingVisible")==false);
    CHECK(all[0].at("DeveloperData")==Json{{"MatchState","1"}});
    CHECK(all[0].at("Users")==Json::array());
    CHECK_FALSE(all[0].contains("SessionKey"));
    CHECK(listener.verifier.calls==0);
    CHECK(result(client.Get(individual),401).is_null());
    const auto credentials=login(client); const auto authorization=headers(credentials);
    CHECK(result(client.Get("/session",authorization),404).is_null());
    CHECK(result(client.Get(individual,authorization),200)==all[0]);
    const auto session=result(client.Get("/session",authorization),200);
    CHECK(session.at("AssignedServerGuid")==std::string(local_guid));
    CHECK(session.at("AssignedServerIp")=="127.0.0.1");
    CHECK(session.at("AssignedServerPort")==listener.runtime.port());
    CHECK(session.at("ClientAdvertisementGuid")=="");
    CHECK(session.at("UserGuid")==credentials.at("UserId"));
    CHECK(result(client.Get(individual,authorization),200)==all[0]);
    CHECK(result(client.Get("/session",authorization),200)==session);
    CHECK(listener.verifier.calls==1);
    CHECK_FALSE(listener.runtime.diagnostics().hello_validated);
    CHECK_FALSE(listener.runtime.diagnostics().world_published);
}
TEST_CASE("Local trial identity-only runtime does not expose a directory snapshot") {
    TrialListener listener(false); httplib::Client client("127.0.0.1",listener.port());
    REQUIRE(listener.runtime.running());
    CHECK(result(client.Get("/gameServerListings"),404).is_null());
    CHECK(result(client.Get(individual),404).is_null());
    const auto credentials=login(client);
    CHECK(result(client.Get("/session",headers(credentials)),404).is_null());
}
TEST_CASE("Local trial selection fails closed after the actual UDP listener stops") {
    TrialListener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials=login(client); const auto authorization=headers(credentials);
    REQUIRE(result(client.Get(individual,authorization),200).is_object());
    REQUIRE(result(client.Get("/session",authorization),200).is_object());
    listener.runtime.stop();
    CHECK_FALSE(listener.runtime.running());
    CHECK(result(client.Get(individual,authorization),503).is_null());
    CHECK(result(client.Get("/session",authorization),503).is_null());
}
