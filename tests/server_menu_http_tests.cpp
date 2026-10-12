#include "hawken/inventory_service.hpp"
#include "hawken/menu_sync_service.hpp"
#include "hawken/server_http.hpp"
#include "hawken/sync_service.hpp"
#include "test_support.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace {
// Only this test translation unit substitutes the external Steam verifier.
class Verifier final : public hawken::auth::SteamVerifier {
public:
    std::atomic<bool> active{false};
    std::atomic<int> calls{0};
    hawken::auth::SteamAuthResult verify(std::span<const std::uint8_t> ticket) override {
        ++calls;
        active = ticket.size() == 4 && ticket[0] == 0xaa && ticket[1] == 0xbb
            && ticket[2] == 1 && ticket[3] == 0;
        return {active ? hawken::auth::SteamAuthStatus::authenticated
                       : hawken::auth::SteamAuthStatus::invalid_ticket,
                active ? 76561197960265729ULL : 0};
    }
    bool is_authenticated(std::uint64_t id) const override {
        return active && id == 76561197960265729ULL;
    }
    void end_session(std::uint64_t) override { active = false; }
};
hawken::inventory::StarterCatalogueIds ids() {
    return {"10000000-0000-0000-0000-000000000001", "10000000-0000-0000-0000-000000000002",
            "10000000-0000-0000-0000-000000000003", "10000000-0000-0000-0000-000000000004",
            "10000000-0000-0000-0000-000000000005", "10000000-0000-0000-0000-000000000006",
            "10000000-0000-0000-0000-000000000007", "10000000-0000-0000-0000-000000000008",
            "10000000-0000-0000-0000-000000000009"};
}
hawken::inventory::StarterBalanceProfile balance() {
    return {"starter.menu-http", {2, 0.25f, 0.1f, 10000, 8, 0, 8, 10, 2, 10000, 90},
            {20, 0, 1.5f, 16000, 80, 0, 80, 1.5f, 20, 16000, 100}, {30, 1, 100}, {2500, 2500, 0, 80, 300, 100, 20}};
}
class Listener {
public:
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store{temporary.path() / "accounts.sqlite", "menu-http"};
    Verifier verifier;
    std::atomic<std::int64_t> now{1000};
    hawken::auth::AuthService auth{store, verifier, [this] { return now.load(); }};
    hawken::sync::SyncService sync{store, auth, 25000};
    hawken::inventory::InventoryService inventory{store, auth, ids(), balance(),
        "20000000-0000-0000-0000-000000000001"};
    hawken::sync::MenuSyncService menu{auth, store};
    explicit Listener(bool enable_menu = true) {
        hawken::server::configure_server_http(server_, &auth, &sync, &inventory,
                                              enable_menu ? &menu : nullptr);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Fixture could not bind loopback.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) throw std::runtime_error("Fixture listener did not start.");
    }
    ~Listener() { server_.stop(); }
    int port() const { return port_; }
private:
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};
struct Credentials { std::string guid, grant; };
nlohmann::json body(const httplib::Result& response, int http_status, int api_status) {
    REQUIRE(response);
    REQUIRE(response->status == http_status);
    CHECK(response->get_header_value("Connection") == "close");
    CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    const auto value = nlohmann::json::parse(response->body);
    CHECK(value.at("Status") == api_status);
    CHECK(value.at("Message").is_string());
    return value.at("Result");
}
Credentials login(httplib::Client& client) {
    const auto result = body(client.Post("/steam/login", R"({"SteamAuthTicket":"AABB0100"})",
                                        "application/json"), 200, 200);
    return {result.at("UserId").get<std::string>(), result.at("AccessGrant").get<std::string>()};
}
httplib::Headers headers(const Credentials& credentials) {
    return {{"Authorization", "Basic " + credentials.grant}};
}
std::array<std::string, 6> targets(const std::string& guid) {
    return {"/userPublicReadOnlyData/" + guid, "/userGameSettings/" + guid,
            "/userGameItemStats/" + guid, "/userStats/" + guid, "/bundles/",
            "/userAchievements/" + guid + "?countryCode=EU"};
}
}

TEST_CASE("Menu HTTP checks authentication before returning absent account data") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    for (const auto& path : targets("01234567-89ab-cdef-0123-456789abcdef"))
        CHECK(body(client.Get(path), 401, 401).is_null());
    CHECK(listener.verifier.calls == 0);
    CHECK_FALSE(listener.store.find_account("76561197960265729"));
}

TEST_CASE("Menu HTTP preserves six recovered DTOs and HTTP200 API404 absence") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto paths = targets(credentials.guid);
    for (const auto index : {0U, 1U, 3U})
        CHECK(body(client.Get(paths.at(index), headers(credentials)), 200, 200)
              == nlohmann::json::object());
    for (const auto index : {2U, 4U})
        CHECK(body(client.Get(paths.at(index), headers(credentials)), 200, 200)
              == nlohmann::json::array());
    const auto absent = client.Get(paths.at(5), headers(credentials));
    CHECK(body(absent, 200, 404) == nlohmann::json::array());
    CHECK(nlohmann::json::parse(absent->body).at("Message") == "Resource not found.");
    const auto account = listener.store.find_account("76561197960265729");
    REQUIRE(account.has_value());
    CHECK_FALSE(listener.store.starter_items(account->ref));
    CHECK(listener.store.visits(account->ref).empty());
    CHECK(listener.store.wallet(account->ref) == hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Menu HTTP rejects methods raw aliases and detail POST without grants") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto paths = targets(credentials.guid);
    for (const auto& path : paths) {
        const auto response = client.Post(path, headers(credentials), "{}", "application/json");
        CHECK(body(response, 405, 405).is_null());
        CHECK(response->get_header_value("Allow") == (path == paths.at(1) ? "GET, PUT" : "GET"));
    }
    // Preserve these raw targets; the test client otherwise normalizes queries.
    client.set_path_encode(false);
    for (const auto& path : std::vector<std::string>{"/bundles", "/bundles//", "/bundles/?x=1",
            "/userStats/" + credentials.guid + "/", "/userGameSettings/" + credentials.guid + "?x=1",
            "/userGameItemStats/%30" + credentials.guid.substr(1),
            "/userAchievements/" + credentials.guid,
            "/userAchievements/" + credentials.guid + "?countryCode=",
            "/userAchievements/" + credentials.guid + "?countryCode=EU&countryCode=EU",
            "/userAchievements/" + credentials.guid + "?countryCode=%45U"}) {
        CAPTURE(path);
        CHECK(body(client.Get(path, headers(credentials)), 404, 404).is_null());
    }
    CHECK(body(client.Post("/userAchievementsQuery/" + credentials.guid + "?countryCode=EU",
                           headers(credentials), "[]", "application/json"), 404, 404).is_null());
    const auto account = listener.store.find_account("76561197960265729");
    REQUIRE(account.has_value());
    CHECK_FALSE(listener.store.starter_items(account->ref));
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Menu HTTP binds user GUID and rejects revocation before resource absence") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : targets("00000000-0000-0000-0000-000000000000")) {
        if (path == "/bundles/") continue;
        CHECK(body(client.Get(path, headers(credentials)), 401, 401).is_null());
    }
    listener.verifier.active = false;
    for (const auto& path : targets(credentials.guid))
        CHECK(body(client.Get(path, headers(credentials)), 401, 401).is_null());
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Menu HTTP logout invalidates reads while keeping owned items and old routes") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    CHECK(body(client.Get("/status/services/"), 200, 200).is_string());
    CHECK(body(client.Get("/users/" + credentials.guid, headers(credentials)), 200, 200).is_object());
    CHECK(body(client.Get("/gameCurrency/" + credentials.guid, headers(credentials)), 200, 200) == 0);
    CHECK(body(client.Get("/game-client/game-items", headers(credentials)), 200, 200).size() == 9);
    const auto owned = body(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200, 200);
    REQUIRE(owned.size() == 9);
    const auto account = listener.store.find_account("76561197960265729");
    REQUIRE(account.has_value());
    const auto persisted = listener.store.starter_items(account->ref);
    REQUIRE(persisted.has_value());
    CHECK(body(client.Put("/users/" + credentials.guid + "/accessGrant", headers(credentials),
                          nlohmann::json{{"AccessGrant", credentials.grant}}.dump(),
                          "application/json"), 200, 200).is_null());
    CHECK_FALSE(listener.verifier.active);
    for (const auto& path : targets(credentials.guid))
        CHECK(body(client.Get(path, headers(credentials)), 401, 401).is_null());
    CHECK(listener.store.starter_items(account->ref) == persisted);
    CHECK(listener.store.visits(account->ref).empty());
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Null optional menu facade preserves prior routing without synthetic successes") {
    Listener listener(false);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : targets(credentials.guid))
        CHECK(body(client.Get(path, headers(credentials)), 404, 404).is_null());
    CHECK(body(client.Get("/game-client/game-items", headers(credentials)), 200, 200).size() == 9);
    CHECK(body(client.Get("/gameCurrency/" + credentials.guid, headers(credentials)), 200, 200) == 0);
    CHECK(body(client.Get("/users/" + credentials.guid, headers(credentials)), 200, 200).is_object());
    CHECK(listener.verifier.calls == 1);
}

namespace {
httplib::ContentProviderWithoutLength chunks(const std::string& data) {
    return [&data](std::size_t offset,httplib::DataSink& sink) {
        const auto count=std::min<std::size_t>(512,data.size()-offset);
        if(count && !sink.write(data.data()+offset,count)) return false;
        if(offset+count==data.size()) sink.done();
        return true;
    };
}
// Raw requests guarantee a single framing mode and deliberately omit body:
// early rejection MUST close without draining a chunk or declared length.
std::string raw_rejection(int port,const std::string& request) {
#ifdef _WIN32
    struct Runtime { Runtime(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0) throw std::runtime_error("Fixture runtime failed.");} ~Runtime(){WSACleanup();}} runtime;
    using Socket=SOCKET;constexpr Socket invalid=INVALID_SOCKET;
#else
    using Socket=int;constexpr Socket invalid=-1;
#endif
    struct Owner {
        Socket value=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        ~Owner(){if(value!=invalid){
#ifdef _WIN32
            closesocket(value);
#else
            ::close(value);
#endif
        }}
    } owner;
    if(owner.value==invalid) throw std::runtime_error("Fixture socket failed.");
#ifdef _WIN32
    const DWORD timeout=2000; const auto* timeout_data=reinterpret_cast<const char*>(&timeout);
#else
    const timeval timeout{2,0};const auto* timeout_data=&timeout;
#endif
    if(setsockopt(owner.value,SOL_SOCKET,SO_RCVTIMEO,timeout_data,static_cast<int>(sizeof(timeout)))!=0 ||
       setsockopt(owner.value,SOL_SOCKET,SO_SNDTIMEO,timeout_data,static_cast<int>(sizeof(timeout)))!=0)
        throw std::runtime_error("Fixture timeout failed.");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<unsigned short>(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(owner.value,reinterpret_cast<const sockaddr*>(&address),static_cast<int>(sizeof(address)))!=0) throw std::runtime_error("Fixture connect failed.");
    for(std::size_t sent=0;sent<request.size();) {
#ifdef _WIN32
        const auto count=::send(owner.value,request.data()+sent,static_cast<int>(request.size()-sent),0);
#else
        const auto count=::send(owner.value,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);
#endif
        if(count<=0) throw std::runtime_error("Fixture send failed.");sent+=static_cast<std::size_t>(count);
    }
    std::string response;char buffer[4096];
    for(;;) {
        const auto count=::recv(owner.value,buffer,static_cast<int>(sizeof(buffer)),0);
        if(count==0) break;
        if(count<0) throw std::runtime_error("Early rejection did not close before body.");
        if(static_cast<std::size_t>(count)>16384-response.size()) throw std::runtime_error("Fixture response exceeded cap.");
        response.append(buffer,static_cast<std::size_t>(count));
    }
    return response;
}
void check_raw_status(const std::string& response,int status) {
    CHECK(response.starts_with("HTTP/1.1 "+std::to_string(status)+" "));
    CHECK(response.find("Connection: close\r\n")!=std::string::npos);
}
}
TEST_CASE("Settings HTTP persists literal decisions and merges both SDK sized batches") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto path="/userGameSettings/"+credentials.guid;
    // Synthetic opaque data from a request, never automatically seeded as consent.
    const nlohmann::json first{{"Fixture.Decision","received"},{"Assignments.Progress",R"({"step":"A"})"}};
    CHECK(body(client.Put(path,headers(credentials),first.dump(),"application/json"),200,200).is_null());
    const nlohmann::json second{{"Fixture.Large",std::string(12000,'v')}};const auto serialized=second.dump();
    REQUIRE(serialized.size()>8192);CHECK(body(client.Put(path,headers(credentials),serialized,"application/json"),200,200).is_null());
    const nlohmann::json expected{{"Fixture.Decision","received"},{"Assignments.Progress",R"({"step":"A"})"},{"Fixture.Large",std::string(12000,'v')}};
    CHECK(body(client.Get(path,headers(credentials)),200,200)==expected);
    CHECK(body(client.Put(path,headers(credentials),"{}","application/json"),200,200).is_null());
    CHECK(body(client.Get(path,headers(credentials)),200,200)==expected);
}
TEST_CASE("Settings HTTP reads chunked updates with the same merge and exclusive 64KiB cap") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto path="/userGameSettings/"+credentials.guid;
    const auto serialized=nlohmann::json{{"Fixture.Chunked",std::string(12000,'c')}}.dump();
    CHECK(body(client.Put(path,headers(credentials),chunks(serialized),"application/json"),200,200).is_null());
    CHECK(body(client.Get(path,headers(credentials)),200,200)==nlohmann::json{{"Fixture.Chunked",std::string(12000,'c')}});
    const auto oversized=std::string(65537,' ');
    CHECK(body(client.Put(path,headers(credentials),chunks(oversized),"application/json"),413,413).is_null());
    CHECK(body(client.Put(path,headers(credentials),oversized,"application/json"),413,413).is_null());
}
TEST_CASE("Settings HTTP rejects types duplicate keys invalid media auth and aliases without writes") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto path="/userGameSettings/"+credentials.guid;
    for(const auto* invalid:{"[]",R"({"x":1})",R"({"x":"one","x":"two"})"})
        CHECK(body(client.Put(path,headers(credentials),invalid,"application/json"),400,400).is_null());
    CHECK(body(client.Put(path,headers(credentials),"{}","text/plain"),400,400).is_null());
    auto duplicate=headers(credentials);duplicate.emplace("Authorization","Basic private-duplicate");
    CHECK(body(client.Put(path,duplicate,"{}","application/json"),400,400).is_null());
    CHECK(body(client.Put(path,"{}","application/json"),401,401).is_null());
    CHECK(body(client.Put("/userGameSettings/00000000-0000-0000-0000-000000000000",headers(credentials),"{}","application/json"),401,401).is_null());
    client.set_path_encode(false);
    for(const auto& alias:std::vector<std::string>{path+"/",path+"?x=1","/userGameSettings/%30"+credentials.guid.substr(1)})
        CHECK(body(client.Put(alias,headers(credentials),"{}","application/json"),404,404).is_null());
    CHECK(body(client.Get(path,headers(credentials)),200,200)==nlohmann::json::object());
    listener.verifier.active=false;
    CHECK(body(client.Put(path,headers(credentials),R"({"x":"v"})","application/json"),401,401).is_null());
}
TEST_CASE("Other HTTP routes keep 8192 read cap for length and chunk framing without invoking Steam") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());
    const auto oversized=std::string(8193,'x');
    CHECK(body(client.Post("/steam/login",oversized,"application/json"),413,413).is_null());
    CHECK(body(client.Post("/steam/login",chunks(oversized),"application/json"),413,413).is_null());
    CHECK(body(client.Put("/users/01234567-89ab-cdef-0123-456789abcdef/accessGrant",oversized,"application/json"),413,413).is_null());
    CHECK(body(client.Put("/users/01234567-89ab-cdef-0123-456789abcdef/accessGrant",chunks(oversized),"application/json"),413,413).is_null());
    CHECK(listener.verifier.calls==0);
}
TEST_CASE("HTTP nonsettings length rejection and chunked early rejection never drain bodies") {
    Listener listener;
    for(const auto* prefix:{"GET /status/services/", "HEAD /status/services/", "GET /unknown", "POST /unknown"}) {
        const auto request=std::string(prefix)+" HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 8193\r\nConnection: close\r\n\r\n";
        check_raw_status(raw_rejection(listener.port(),request),413);
    }
    for(const auto& row:std::vector<std::pair<std::string,int>>{{"GET /status/services/",400},{"HEAD /status/services/",400},
        {"POST /unknown",404},{"POST /userGameSettings/01234567-89ab-cdef-0123-456789abcdef",405}}) {
        const auto request=row.first+" HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        check_raw_status(raw_rejection(listener.port(),request),row.second);
    }
    CHECK(listener.verifier.calls==0);CHECK_FALSE(listener.store.find_account("76561197960265729"));
}
