#include "hawken/inventory_service.hpp"
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
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
class TestVerifier final : public hawken::auth::SteamVerifier {
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
    return {"starter.http-test", {2, 0.25f, 0.1f, 10000, 8, 0, 8, 10, 2, 10000, 90},
            {20, 0, 1.5f, 16000, 80, 0, 80, 1.5f, 20, 16000, 100}, {30, 1, 100}, {2500, 2500, 0, 80, 300, 100, 20}};
}
class Listener {
public:
    hawken::test::TempDirectory temporary;
    hawken::store::LocalStore store;
    TestVerifier verifier;
    std::int64_t now = 1000;
    hawken::auth::AuthService auth{store, verifier, [this] { return now; }};
    hawken::sync::SyncService sync{store, auth, 25000};
    std::unique_ptr<hawken::inventory::InventoryService> inventory;
    explicit Listener(bool enable_inventory = true, const std::filesystem::path& database_path = {},
                      hawken::server::BrokerHttpRejectObserver reject_observer = {},
                      hawken::inventory::PutObserver put_observer = {})
        : store(database_path.empty() ? temporary.path() / "accounts.sqlite" : database_path, "http-inventory") {
        if (enable_inventory)
            inventory = std::make_unique<hawken::inventory::InventoryService>(store, auth, ids(), balance(),
                "20000000-0000-0000-0000-000000000001", std::move(put_observer));
        hawken::server::configure_server_http(server_, &auth, &sync, inventory.get(), nullptr, nullptr,
            nullptr, std::move(reject_observer));
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
Credentials login(httplib::Client& client) {
    const auto response = client.Post("/steam/login", R"({"SteamAuthTicket":"AABB0100"})", "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    return {body.at("Result").at("UserId").get<std::string>(),
            body.at("Result").at("AccessGrant").get<std::string>()};
}
httplib::Headers headers(const Credentials& credentials) {
    return {{"Authorization", "Basic " + credentials.grant}};
}
std::array<std::string, 3> targets(const Credentials& credentials) {
    return {"/game-client/game-items", "/game-client/game-offers", "/userGameItems/" + credentials.guid};
}
nlohmann::json result(const httplib::Result& response, int status) {
    REQUIRE(response);
    REQUIRE(response->status == status);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body.at("Status") == status);
    CHECK(response->get_header_value("Connection") == "close");
    CHECK(response->get_header_value("Content-Type") == "application/json; charset=utf-8");
    return body.at("Result");
}
}

TEST_CASE("Inventory HTTP rejects missing grants before any account is created") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const Credentials unused{"01234567-89ab-cdef-0123-456789abcdef", ""};
    for (const auto& path : targets(unused)) CHECK(result(client.Get(path), 401).is_null());
    CHECK(listener.verifier.calls == 0);
    CHECK_FALSE(listener.store.find_account("76561197960265729"));
}

TEST_CASE("HTTP catalogue offers and persisted owned inventory share the same nine types") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto account = listener.store.find_account("76561197960265729");
    REQUIRE(account.has_value());
    const auto catalogue = result(client.Get("/game-client/game-items", headers(credentials)), 200);
    const auto offers = result(client.Get("/game-client/game-offers", headers(credentials)), 200);
    REQUIRE(catalogue.is_array());
    REQUIRE(catalogue.size() == 9);
    CHECK(catalogue.at(2).at("Proj.Speed") == "2500");
    CHECK(catalogue.at(2).at("Proj.MaxSpeed") == "2500");
    CHECK(catalogue.at(2).at("Proj.AccelRate") == "0");
    CHECK(catalogue.at(2).at("Proj.Damage") == "80");
    CHECK(catalogue.at(2).at("Proj.DamageRadius") == "300");
    CHECK(catalogue.at(2).at("Proj.InnerDamageRadius") == "100");
    CHECK(catalogue.at(2).at("Proj.MinDamage") == "20");
    CHECK_FALSE(catalogue.at(1).contains("Proj.Speed"));
    REQUIRE(offers.is_array());
    REQUIRE(offers.size() == 1);
    CHECK(offers.at(0).at("Fees") == nlohmann::json{{"HP", 0}});
    CHECK_FALSE(listener.store.starter_items(account->ref));
    const auto owned = result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200);
    REQUIRE(owned.is_array());
    REQUIRE(owned.size() == 9);
    const auto stored = listener.store.starter_items(account->ref);
    REQUIRE(stored.has_value());
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(owned.at(i).at("GameItemsGuid") == catalogue.at(i).at("GameItemsGuid"));
        CHECK(offers.at(0).at("GameItems").at(i).at("GameItemsGuid") == catalogue.at(i).at("GameItemsGuid"));
        CHECK(owned.at(i).at("UserGameItemGuid") == stored->at(i).instance_id);
        CHECK(owned.at(i).at("Version") == "starter.http-test");
    }
    CHECK(owned.at(0).at("Preset.PrimaryWeapon_InstanceId_0") == stored->at(1).instance_id);
    CHECK(listener.store.wallet(account->ref) == hawken::store::WalletBalances{});
    CHECK(result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200) == owned);
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Inventory HTTP rejects raw aliases methods and purchase routes without granting") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    const auto paths = targets(credentials);
    for (const auto& path : paths) {
        for (const auto& alias : {path + "/", path + "?x=1"})
            CHECK(result(client.Get(alias, headers(credentials)), 404).is_null());
        const auto response = client.Post(path, headers(credentials), "{}", "application/json");
        CHECK(result(response, 405).is_null());
        CHECK(response->get_header_value("Allow") == "GET");
    }
    for (const auto& path : std::vector<std::string>{"/game-client/%67ame-items", "/Game-client/game-offers",
            "/userGameItems/%" + std::string("30") + credentials.guid.substr(1),
            "/game-client/game-offers/purchase"})
        CHECK(result(client.Get(path, headers(credentials)), 404).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
    CHECK(listener.verifier.calls == 1);
}

TEST_CASE("Owned HTTP checks the path account and every inventory route checks revocation") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    CHECK(result(client.Get("/userGameItems/00000000-0000-0000-0000-000000000000", headers(credentials)), 401).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
    listener.verifier.active = false;
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 401).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
}

TEST_CASE("Logout revokes HTTP catalogue access while owned items stay persisted") {
    Listener listener;
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    REQUIRE(result(client.Get("/userGameItems/" + credentials.guid, headers(credentials)), 200).size() == 9);
    CHECK(result(client.Put("/users/" + credentials.guid + "/accessGrant", headers(credentials),
        nlohmann::json{{"AccessGrant", credentials.grant}}.dump(), "application/json"), 200).is_null());
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 401).is_null());
    REQUIRE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref).has_value());
}

TEST_CASE("Absent inventory facade preserves previous HTTP bootstrap auth and sync routes") {
    Listener listener(false);
    httplib::Client client("127.0.0.1", listener.port());
    const auto credentials = login(client);
    for (const auto& path : targets(credentials))
        CHECK(result(client.Get(path, headers(credentials)), 404).is_null());
    CHECK(result(client.Get("/gameCurrency/" + credentials.guid, headers(credentials)), 200) == 0);
    CHECK(result(client.Get("/users/" + credentials.guid, headers(credentials)), 200).at("Guid") == credentials.guid);
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
}

namespace {
std::string broker(const Credentials& credentials,const std::string& instance) {
    return "/userGameItemsBroker/"+credentials.guid+"/"+instance;
}
httplib::ContentProviderWithoutLength chunks(const std::string& data) {
    return [&data](std::size_t offset,httplib::DataSink& sink) {
        const auto count=std::min<std::size_t>(512,data.size()-offset);
        if(count && !sink.write(data.data()+offset,count))return false;
        if(offset+count==data.size())sink.done();
        return true;
    };
}
// No body is sent: early framing/method rejection must close without draining.
std::string raw_rejection(int port,const std::string& request) {
#ifdef _WIN32
    struct Runtime {
        Runtime(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0)throw std::runtime_error("Fixture runtime failed.");}
        ~Runtime(){WSACleanup();}
    } runtime;
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
    if(owner.value==invalid)throw std::runtime_error("Fixture socket failed.");
#ifdef _WIN32
    const DWORD timeout=2000;const auto* timeout_data=reinterpret_cast<const char*>(&timeout);
#else
    const timeval timeout{2,0};const auto* timeout_data=&timeout;
#endif
    if(setsockopt(owner.value,SOL_SOCKET,SO_RCVTIMEO,timeout_data,static_cast<int>(sizeof(timeout)))!=0 ||
       setsockopt(owner.value,SOL_SOCKET,SO_SNDTIMEO,timeout_data,static_cast<int>(sizeof(timeout)))!=0)
        throw std::runtime_error("Fixture timeout failed.");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<unsigned short>(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(owner.value,reinterpret_cast<const sockaddr*>(&address),static_cast<int>(sizeof(address)))!=0)
        throw std::runtime_error("Fixture connect failed.");
    for(std::size_t sent=0;sent<request.size();) {
#ifdef _WIN32
        const auto count=::send(owner.value,request.data()+sent,static_cast<int>(request.size()-sent),0);
#else
        const auto count=::send(owner.value,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);
#endif
        if(count<=0)throw std::runtime_error("Fixture send failed.");
        sent+=static_cast<std::size_t>(count);
    }
    std::string response;char buffer[4096];
    for(;;) {
        const auto count=::recv(owner.value,buffer,static_cast<int>(sizeof(buffer)),0);
        if(count==0)break;
        if(count<0)throw std::runtime_error("Early rejection did not close before body.");
        if(static_cast<std::size_t>(count)>16384-response.size())throw std::runtime_error("Fixture response exceeded cap.");
        response.append(buffer,static_cast<std::size_t>(count));
    }
    return response;
}
void raw_status(const std::string& response,int status) {
    CHECK(response.starts_with("HTTP/1.1 "+std::to_string(status)+" "));
    CHECK(response.find("Connection: close\r\n")!=std::string::npos);
}
}
TEST_CASE("Broker HTTP commits a void ACK and overlays only saved Presets on the owned list") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    REQUIRE(owned.size()==9);const auto instance=owned.at(0).at("UserGameItemGuid").get<std::string>();
    const auto path=broker(credentials,instance);auto expected=owned;
    expected.at(0)["Preset.HttpFixture"]="opaque-received";
    CHECK(result(client.Put(path,headers(credentials),R"({"Preset.HttpFixture":"opaque-received"})","application/json"),200).is_null());
    CHECK(result(client.Put(path,headers(credentials),R"({"Preset.HttpFixture":"opaque-received"})","application/json"),200).is_null());
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    CHECK(listener.store.owned_item_attributes(account->ref,instance)==hawken::store::ItemAttributes{{"Preset.HttpFixture","opaque-received"}});
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls==1);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid+"/"+instance,headers(credentials)),404).is_null());
}
TEST_CASE("Broker HTTP saved item persists across listener store and authentication restart") {
    hawken::test::TempDirectory temporary;const auto database=temporary.path()/"accounts.sqlite";
    Credentials old;std::string instance;nlohmann::json expected;
    {
        Listener listener(true,database);httplib::Client client("127.0.0.1",listener.port());old=login(client);
        expected=result(client.Get("/userGameItems/"+old.guid,headers(old)),200);REQUIRE(expected.size()==9);
        instance=expected.at(0).at("UserGameItemGuid").get<std::string>();
        expected.at(0)["Preset.RestartHttpFixture"]="received-not-seeded";
        CHECK(result(client.Put(broker(old,instance),headers(old),R"({"Preset.RestartHttpFixture":"received-not-seeded"})","application/json"),200).is_null());
    }
    Listener listener(true,database);httplib::Client client("127.0.0.1",listener.port());const auto fresh=login(client);
    CHECK(fresh.guid==old.guid);CHECK(fresh.grant!=old.grant);
    CHECK(result(client.Put(broker(old,instance),headers(old),"{}","application/json"),401).is_null());
    CHECK(result(client.Get("/userGameItems/"+fresh.guid,headers(fresh)),200)==expected);
    CHECK(result(client.Put(broker(fresh,instance),headers(fresh),"{}","application/json"),200).is_null());
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    REQUIRE(listener.store.starter_items(account->ref));CHECK(listener.store.starter_items(account->ref)->size()==9);
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
TEST_CASE("Broker HTTP requires owned mech grant and rechecks account and revocation") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const std::string unknown="30000000-0000-0000-0000-000000000099";
    CHECK(result(client.Put(broker(credentials,unknown),"{}","application/json"),401).is_null());
    CHECK(result(client.Put(broker(credentials,unknown),headers(credentials),"{}","application/json"),404).is_null());
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);CHECK_FALSE(listener.store.starter_items(account->ref));
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);REQUIRE(owned.size()==9);
    const auto path=broker(credentials,owned.at(0).at("UserGameItemGuid").get<std::string>());
    CHECK(result(client.Put(broker(credentials,owned.at(1).at("UserGameItemGuid").get<std::string>()),headers(credentials),"{}","application/json"),404).is_null());
    Credentials foreign{"30000000-0000-0000-0000-000000000098",credentials.grant};
    CHECK(result(client.Put(broker(foreign,unknown),headers(credentials),"{}","application/json"),401).is_null());
    listener.verifier.active=false;
    CHECK(result(client.Put(path,headers(credentials),R"({"Preset.Fixture":"x"})","application/json"),401).is_null());
    CHECK(listener.store.owned_item_attributes(account->ref,owned.at(0).at("UserGameItemGuid").get<std::string>()).empty());
}
TEST_CASE("Broker HTTP rejects method raw aliases duplicate headers and invalid JSON atomically") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);REQUIRE(owned.size()==9);
    const auto path=broker(credentials,owned.at(0).at("UserGameItemGuid").get<std::string>());
    for(const auto& response:{client.Get(path,headers(credentials)),client.Post(path,headers(credentials),"{}","application/json")}) {
        CHECK(result(response,405).is_null());CHECK(response->get_header_value("Allow")=="PUT");
    }
    client.set_path_encode(false);
    for(const auto& alias:std::vector<std::string>{path+"/",path+"?x=1",path+"/extra","/userGameItemsBroker/%30"+credentials.guid.substr(1)+"/"+owned.at(0).at("UserGameItemGuid").get<std::string>()})
        CHECK(result(client.Put(alias,headers(credentials),"{}","application/json"),404).is_null());
    CHECK(result(client.Put(path,headers(credentials),"{}","text/plain"),400).is_null());
    auto duplicate=headers(credentials);duplicate.emplace("Authorization","Basic private-duplicate");
    CHECK(result(client.Put(path,duplicate,"{}","application/json"),400).is_null());
    for(const auto* json:{R"({"Preset.Fixture":1})",R"({"Preset.Fixture":{}})",R"({"Preset.Fixture":"a","Preset.Fixture":"b"})",R"({"Preset.Fixture":"a","ReadOnly_Children":"changed"})"})
        CHECK(result(client.Put(path,headers(credentials),json,"application/json"),400).is_null());
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==owned);
}
TEST_CASE("Exact Broker PUT alone accepts bounded large length and chunked SDK batches") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);REQUIRE(owned.size()==9);
    const auto path=broker(credentials,owned.at(0).at("UserGameItemGuid").get<std::string>());
    const auto value=nlohmann::json{{"Preset.HttpLargeFixture",std::string(12000,'v')}}.dump();REQUIRE(value.size()>8192);
    CHECK(result(client.Put(path,headers(credentials),value,"application/json"),200).is_null());
    CHECK(result(client.Put(path,headers(credentials),chunks(value),"application/json"),200).is_null());
    auto exact=value+std::string(65536-value.size(),' ');
    CHECK(result(client.Put(path,headers(credentials),exact,"application/json"),200).is_null());
    CHECK(result(client.Put(path,headers(credentials),chunks(exact),"application/json"),200).is_null());
    CHECK(result(client.Put(path,headers(credentials),exact+" ","application/json"),413).is_null());
    const auto oversized=exact+" ";CHECK(result(client.Put(path,headers(credentials),chunks(oversized),"application/json"),413).is_null());
    CHECK(result(client.Post("/steam/login",std::string(8193,'x'),"application/json"),413).is_null());
    const auto other=std::string(8193,'x');CHECK(result(client.Post("/steam/login",chunks(other),"application/json"),413).is_null());
    CHECK(listener.verifier.calls==1);
    const auto after=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    CHECK(after.at(0).contains("Preset.HttpLargeFixture"));
    if(after.at(0).contains("Preset.HttpLargeFixture"))
        CHECK(after.at(0).at("Preset.HttpLargeFixture")==std::string(12000,'v'));
}
TEST_CASE("Broker framing rejections close without draining and never expand other routes") {
    Listener listener;const std::string path="/userGameItemsBroker/01234567-89ab-cdef-0123-456789abcdef/30000000-0000-0000-0000-000000000001";
    for(const auto& [target,status]:std::vector<std::pair<std::string,int>>{{"PUT "+path,413},{"POST "+path,413},{"PUT "+path+"/",413}}) {
        const auto declared=target.starts_with("PUT ") && !target.ends_with('/') ? 65537 : 8193;
        raw_status(raw_rejection(listener.port(),target+" HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "+std::to_string(declared)+"\r\nConnection: close\r\n\r\n"),status);
    }
    for(const auto& [method,status]:std::vector<std::pair<std::string,int>>{{"GET",400},{"HEAD",400},{"POST",405}})
        raw_status(raw_rejection(listener.port(),method+" "+path+" HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"),status);
    CHECK(listener.verifier.calls==0);CHECK_FALSE(listener.store.find_account("76561197960265729"));
}
TEST_CASE("Absent inventory facade never enables Broker writes or grants") {
    Listener listener(false);httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    CHECK(result(client.Put(broker(credentials,"30000000-0000-0000-0000-000000000099"),headers(credentials),"{}","application/json"),404).is_null());
    CHECK_FALSE(listener.store.starter_items(listener.store.find_account("76561197960265729")->ref));
}

TEST_CASE("Broker HTTP accepts native helper empty literals with void ACK and exact list readback") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);REQUIRE(before.size()==9);
    const auto instance=before.at(0).at("UserGameItemGuid").get<std::string>();
    // Independent literal request from the thirteen confirmed helper call sites.
    const std::string request=R"({"Preset.Items1_InstanceId":"empty","Preset.Items2_InstanceId":"empty","Preset.Items3_InstanceId":"empty","Preset.Internals1_InstanceId":"empty","Preset.Internals2_InstanceId":"empty","Preset.Internals3_InstanceId":"empty","Preset.Internals4_InstanceId":"empty","Preset.Internals5_InstanceId":"empty","Preset.Internals6_InstanceId":"empty","Preset.Style.AnimTauntType_InstanceId":"empty","Preset.Style.CockpitDecoration1_InstanceId":"empty","Preset.Style.CockpitDecoration2_InstanceId":"empty","Preset.Style.CockpitDecoration3_InstanceId":"empty"})";
    CHECK(result(client.Put(broker(credentials,instance),headers(credentials),request,"application/json"),200).is_null());
    CHECK(result(client.Put(broker(credentials,instance),headers(credentials),request,"application/json"),200).is_null());
    const auto sentinels=nlohmann::json::parse(request);
    auto expected=before;for(const auto& [key,value]:sentinels.items())expected.at(0)[key]=value;
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    const auto attrs=listener.store.owned_item_attributes(account->ref,instance);CHECK(attrs.size()==13);
    for(const auto& [key,value]:attrs){CHECK(key.starts_with("Preset."));CHECK(value=="empty");}
    const auto items=listener.store.starter_items(account->ref);REQUIRE(items);CHECK(items->size()==9);
    for(std::size_t slot=0;slot<9;++slot) {
        CHECK(items->at(slot).instance_id==before.at(slot).at("UserGameItemGuid").get<std::string>());
        CHECK(items->at(slot).type_id==before.at(slot).at("GameItemsGuid").get<std::string>());
    }
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls==1);
}
TEST_CASE("Broker HTTP rejects native empty outside helper keys without partially merging a batch") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);REQUIRE(before.size()==9);
    const auto instance=before.at(0).at("UserGameItemGuid").get<std::string>();
    for(const auto* key:{"Preset.PrimaryWeapon_InstanceId_0","Preset.Style.UpperPart_InstanceId",
        "Preset.Unknown_InstanceId_0","Preset.Items4_InstanceId","Preset.Style.CockpitDecoration0_InstanceId"})
        CHECK(result(client.Put(broker(credentials,instance),headers(credentials),
            nlohmann::json{{"Preset.Fixture","must-not-commit"},{key,"empty"}}.dump(),"application/json"),400).is_null());
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    CHECK(listener.store.owned_item_attributes(account->ref,instance).empty());
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
}

namespace {
class BrokerDiagnostics {
public:
    void add(hawken::server::BrokerHttpRejectReason reason) {
        std::lock_guard lock(mutex_); reasons_.push_back(reason);
    }
    std::vector<hawken::server::BrokerHttpRejectReason> snapshot() const {
        std::lock_guard lock(mutex_); return reasons_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<hawken::server::BrokerHttpRejectReason> reasons_;
};
}
TEST_CASE("Broker HTTP header diagnostics report once without altering rejection or inventory") {
    using Reason=hawken::server::BrokerHttpRejectReason;
    BrokerDiagnostics diagnostics;
    Listener listener(true,{},[&](Reason reason){diagnostics.add(reason);});
    httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    const auto path=broker(credentials,before.at(0).at("UserGameItemGuid").get<std::string>());
    const std::string body=R"({"Preset.DiagnosticFixture":"public-body-marker"})";
    CHECK(result(client.Put(path,headers(credentials),body,"text/plain"),400).is_null());
    CHECK(diagnostics.snapshot()==std::vector<Reason>{Reason::http_headers});
    auto duplicate=headers(credentials);duplicate.emplace("Authorization","public-header-marker");
    CHECK(result(client.Put(path,duplicate,body,"application/json"),400).is_null());
    CHECK(diagnostics.snapshot()==std::vector<Reason>{Reason::http_headers,Reason::http_headers});
    // Service authentication rejection is not an HTTP pre-service rejection.
    CHECK(result(client.Put(path,body,"application/json"),401).is_null());
    CHECK(diagnostics.snapshot()==std::vector<Reason>{Reason::http_headers,Reason::http_headers});
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    CHECK(listener.verifier.calls==1);
}
TEST_CASE("Broker malformed chunk reports reader failure once and never stores its data") {
    using Reason=hawken::server::BrokerHttpRejectReason;
    BrokerDiagnostics diagnostics;
    Listener listener(true,{},[&](Reason reason){diagnostics.add(reason);});
    httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    const auto path=broker(credentials,before.at(0).at("UserGameItemGuid").get<std::string>());
    const auto response=raw_rejection(listener.port(),"PUT "+path+" HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic "+credentials.grant+
        "\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nZ\r\n");
    raw_status(response,400);
    CHECK(diagnostics.snapshot()==std::vector<Reason>{Reason::body_reader});
    CHECK(response.find(credentials.grant)==std::string::npos);
    CHECK(response.find(credentials.guid)==std::string::npos);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    CHECK(listener.verifier.calls==1);
}
TEST_CASE("Throwing Broker HTTP observer cannot change status or prevent later valid writes") {
    using Reason=hawken::server::BrokerHttpRejectReason;
    std::atomic<unsigned> observed{0};
    Listener listener(true,{},[&](Reason){++observed;throw std::runtime_error("public-observer-marker");});
    httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    const auto path=broker(credentials,before.at(0).at("UserGameItemGuid").get<std::string>());
    const auto rejected=client.Put(path,headers(credentials),"public-body-marker","text/plain");
    CHECK(result(rejected,400).is_null());CHECK(observed==1);
    CHECK(rejected->body.find("public-observer-marker")==std::string::npos);
    CHECK(rejected->body.find("public-body-marker")==std::string::npos);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    CHECK(result(client.Put(path,headers(credentials),R"({"Preset.DiagnosticFixture":"public"})","application/json"),200).is_null());
    CHECK(observed==1);
    const auto after=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    auto expected=before;expected.at(0)["Preset.DiagnosticFixture"]="public";CHECK(after==expected);
}

namespace {
class PutDiagnostics {
public:
    void add(const hawken::inventory::PutObservation& observation) {
        std::lock_guard lock(mutex_); observations_.push_back(observation);
    }
    std::vector<hawken::inventory::PutObservation> snapshot() const {
        std::lock_guard lock(mutex_); return observations_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<hawken::inventory::PutObservation> observations_;
};
}
TEST_CASE("Broker reference category reproduces owned helper rejection through loopback") {
    using Reason=hawken::inventory::PutReason;
    PutDiagnostics diagnostics;
    Listener listener(true,{}, {},[&](const hawken::inventory::PutObservation& observation){diagnostics.add(observation);});
    httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    REQUIRE(before.size()==9);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    const auto mech=before.at(0).at("UserGameItemGuid").get<std::string>();
    const auto primary=before.at(1).at("UserGameItemGuid").get<std::string>();
    const auto path=broker(credentials,mech);
    const nlohmann::json valid{{"Preset.PrimaryWeapon_0","0"},
        {"Preset.PrimaryWeapon_InstanceId_0",primary},{"Preset.Items1_InstanceId","empty"}};
    CHECK(result(client.Put(path,headers(credentials),valid.dump(),"application/json"),200).is_null());
    const auto committed=diagnostics.snapshot();REQUIRE(committed.size()==1);
    CHECK(committed.at(0).reason==Reason::success);
    auto expected=before;expected.at(0)["Preset.PrimaryWeapon_0"]="0";
    expected.at(0)["Preset.PrimaryWeapon_InstanceId_0"]=primary;
    expected.at(0)["Preset.Items1_InstanceId"]="empty";
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    const hawken::store::ItemAttributes saved{{"Preset.PrimaryWeapon_0","0"},
        {"Preset.PrimaryWeapon_InstanceId_0",primary},{"Preset.Items1_InstanceId","empty"}};
    REQUIRE(listener.store.owned_item_attributes(account->ref,mech)==saved);

    // Synthetic diagnostic counterexample, not a captured native request or an
    // item22 compatibility claim: reuse an existing primary instance, no grant.
    auto rejected=valid;rejected["Preset.Items1_InstanceId"]=primary;
    rejected["Preset.Ability"]="2";
    const auto response=client.Put(path,headers(credentials),rejected.dump(),"application/json");
    CHECK(result(response,400).is_null());
    const auto observed=diagnostics.snapshot();REQUIRE(observed.size()==2);
    CHECK(observed.at(0).reason==Reason::success);
    CHECK(observed.at(1).reason==Reason::reference_unknown_key);
    CHECK(observed.at(1).reference_family==hawken::inventory::ReferenceFamily::helper_optional);
    CHECK(observed.at(1).value_form==hawken::inventory::ReferenceValueForm::matches_owned);
    CHECK(response->body.find(primary)==std::string::npos);
    CHECK(response->body.find("Preset.Items1_InstanceId")==std::string::npos);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    CHECK(listener.store.owned_item_attributes(account->ref,mech)==saved);
    const auto stored=listener.store.starter_items(account->ref);REQUIRE(stored);CHECK(stored->size()==9);
    for(std::size_t slot=0;slot<9;++slot) {
        CHECK(stored->at(slot).instance_id==before.at(slot).at("UserGameItemGuid").get<std::string>());
        CHECK(stored->at(slot).type_id==before.at(slot).at("GameItemsGuid").get<std::string>());
        if(slot!=0)CHECK(listener.store.owned_item_attributes(account->ref,stored->at(slot).instance_id).empty());
    }
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls==1);
}
TEST_CASE("Broker reference category distinguishes authentication ownership and invalid controls") {
    using Reason=hawken::inventory::PutReason;
    PutDiagnostics diagnostics;
    Listener listener(true,{}, {},[&](const hawken::inventory::PutObservation& observation){diagnostics.add(observation);});
    httplib::Client client("127.0.0.1",listener.port());const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    REQUIRE(before.size()==9);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    const auto mech=before.at(0).at("UserGameItemGuid").get<std::string>();
    const auto primary=before.at(1).at("UserGameItemGuid").get<std::string>();
    const auto path=broker(credentials,mech);
    const auto valid=nlohmann::json{{"Preset.PrimaryWeapon_InstanceId_0",primary}}.dump();
    CHECK(result(client.Put(path,valid,"application/json"),401).is_null());
    Credentials foreign{"30000000-0000-0000-0000-000000000098",credentials.grant};
    CHECK(result(client.Put(broker(foreign,mech),headers(credentials),valid,"application/json"),401).is_null());
    CHECK(result(client.Put(broker(credentials,primary),headers(credentials),valid,"application/json"),404).is_null());
    CHECK(result(client.Put(path,headers(credentials),
        R"({"Preset.PrimaryWeapon_InstanceId_0":"30000000-0000-0000-0000-000000000099"})",
        "application/json"),400).is_null());
    CHECK(result(client.Put(path,headers(credentials),
        R"({"Preset.Items1_InstanceId":1})","application/json"),400).is_null());
    const auto observed=diagnostics.snapshot();
    const std::array<Reason,5> expected{Reason::authentication,Reason::authentication,
        Reason::ownership,Reason::reference_instance,Reason::value_type};
    REQUIRE(observed.size()==expected.size());
    for(std::size_t i=0;i<expected.size();++i)CHECK(observed.at(i).reason==expected.at(i));
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    for(const auto& item:before)
        CHECK(listener.store.owned_item_attributes(account->ref,item.at("UserGameItemGuid").get<std::string>()).empty());
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls==1);
}

TEST_CASE("Broker observed visor defaults persist through real loopback HTTP") {
    Listener listener;httplib::Client client("127.0.0.1",listener.port());
    const auto credentials=login(client);
    const auto before=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200);
    REQUIRE(before.size()==9);
    const auto account=listener.store.find_account("76561197960265729");REQUIRE(account);
    const auto stored_before=listener.store.starter_items(account->ref);REQUIRE(stored_before);
    const auto mech=before.at(0).at("UserGameItemGuid").get<std::string>();
    const auto primary=before.at(1).at("UserGameItemGuid").get<std::string>();
    const auto path=broker(credentials,mech);
    // The five defaults have native provenance; authentication and owned IDs are fixtures.
    const std::string body=R"({"Preset.VisorStyle.FriendHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID","Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2","Preset.VisorStyle.NeutralHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID3","Preset.VisorStyle.PrimaryReticle_InstanceId":"FAKE-PRIM-RETICLE-INSTANCE-GUID","Preset.VisorStyle.SecondaryReticle_InstanceId":"FAKE-SEC-RETICLE-INSTANCE-GUID"})";
    CHECK(result(client.Put(path,body,"application/json"),401).is_null());
    Credentials foreign{"30000000-0000-0000-0000-000000000098",credentials.grant};
    CHECK(result(client.Put(broker(foreign,mech),headers(credentials),body,"application/json"),401).is_null());
    CHECK(result(client.Put(broker(credentials,"30000000-0000-0000-0000-000000000099"),
        headers(credentials),body,"application/json"),404).is_null());
    CHECK(result(client.Put(broker(credentials,primary),headers(credentials),body,"application/json"),404).is_null());
    for(const auto& rejected:std::vector<std::string>{
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2-X"})",
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID"})",
        R"({"Preset.VisorStyle.FriendHUDColor_InstanceId":"FAKE-COLOR-INSTANCE-GUID2"})",
        R"({"Preset.VisorStyle.FoeHUDColor_InstanceId_0":"FAKE-COLOR-INSTANCE-GUID2"})",
        R"({"Preset.PrimaryWeapon_InstanceId_0":"FAKE-COLOR-INSTANCE-GUID2"})"})
        CHECK(result(client.Put(path,headers(credentials),rejected,"application/json"),400).is_null());
    CHECK(listener.store.owned_item_attributes(account->ref,mech).empty());
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==before);
    const auto response=client.Put(path,headers(credentials),body,"application/json");REQUIRE(response);
    CHECK(response->status==200);
    const auto envelope=nlohmann::json::parse(response->body);
    CHECK(envelope.at("Status")==200);CHECK(envelope.at("Result").is_null());
    CHECK(response->get_header_value("Content-Type")=="application/json; charset=utf-8");
    CHECK(response->get_header_value("Connection")=="close");
    const hawken::store::ItemAttributes expected_attributes{
        {"Preset.VisorStyle.FriendHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID"},
        {"Preset.VisorStyle.FoeHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID2"},
        {"Preset.VisorStyle.NeutralHUDColor_InstanceId","FAKE-COLOR-INSTANCE-GUID3"},
        {"Preset.VisorStyle.PrimaryReticle_InstanceId","FAKE-PRIM-RETICLE-INSTANCE-GUID"},
        {"Preset.VisorStyle.SecondaryReticle_InstanceId","FAKE-SEC-RETICLE-INSTANCE-GUID"}};
    CHECK(listener.store.owned_item_attributes(account->ref,mech)==expected_attributes);
    auto expected=before;
    for(const auto& [key,value]:expected_attributes)expected.at(0)[key]=value;
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    const auto replay=client.Put(path,headers(credentials),body,"application/json");REQUIRE(replay);
    CHECK(replay->status==200);const auto replay_envelope=nlohmann::json::parse(replay->body);
    CHECK(replay_envelope.at("Status")==200);CHECK(replay_envelope.at("Result").is_null());
    CHECK(listener.store.owned_item_attributes(account->ref,mech)==expected_attributes);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==expected);
    CHECK(listener.store.starter_items(account->ref)==stored_before);
    for(std::size_t slot=1;slot<before.size();++slot)
        CHECK(listener.store.owned_item_attributes(account->ref,
            before.at(slot).at("UserGameItemGuid").get<std::string>()).empty());
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(listener.verifier.calls==1);
}


TEST_CASE("Item batch HTTP reads requested owned rows in order with UUID case normalization and saved Presets") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port());
    const auto credentials=login(client); const auto account=listener.store.find_account("76561197960265729"); REQUIRE(account);
    auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200); REQUIRE(owned.size()==9);
    const auto mech=owned.at(0).at("UserGameItemGuid").get<std::string>();
    CHECK(result(client.Put(broker(credentials,mech),headers(credentials),R"({"Preset.BatchFixture":"actually-saved"})","application/json"),200).is_null());
    owned.at(0)["Preset.BatchFixture"]="actually-saved";
    const auto saved_items=listener.store.starter_items(account->ref); const auto saved_wallet=listener.store.wallet(account->ref);
    auto uppercase=owned.at(1).at("UserGameItemGuid").get<std::string>();
    for(auto& c:uppercase) if(c>='a' && c<='f') c=static_cast<char>(c-'a'+'A');
    const auto request=nlohmann::json::array({uppercase,mech}).dump()+"\n";
    const auto path="/userGameItemsBatch/"+credentials.guid;
    const auto expected=nlohmann::json::array({owned.at(1),owned.at(0)});
    CHECK(result(client.Post(path,headers(credentials),request,"application/json"),200)==expected);
    CHECK(result(client.Post(path,headers(credentials),chunks(request),"application/json"),200)==expected);
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==owned);
    CHECK(listener.store.starter_items(account->ref)==saved_items);
    CHECK(listener.store.wallet(account->ref)==saved_wallet);
    CHECK(listener.store.owned_item_attributes(account->ref,mech)==hawken::store::ItemAttributes{{"Preset.BatchFixture","actually-saved"}});
    CHECK(listener.verifier.calls==1);
}
TEST_CASE("Item batch HTTP null and empty arrays never grant or enumerate an account inventory") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port()); const auto credentials=login(client);
    const auto account=listener.store.find_account("76561197960265729"); REQUIRE(account);
    const auto path="/userGameItemsBatch/"+credentials.guid;
    // SDK66e60's empty vector is null+LF. [] is our explicit equivalent empty-read policy.
    for(const auto* body:{"null\n","[]\n"}) CHECK(result(client.Post(path,headers(credentials),body,"application/json"),200)==nlohmann::json::array());
    CHECK(result(client.Post(path,headers(credentials),R"(["30000000-0000-0000-0000-000000000099"])","application/json"),404).is_null());
    CHECK_FALSE(listener.store.starter_items(account->ref));
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200); REQUIRE(owned.size()==9);
    const auto saved=listener.store.starter_items(account->ref);
    CHECK(result(client.Post(path,headers(credentials),"null\n","application/json"),200)==nlohmann::json::array());
    CHECK(result(client.Post(path,headers(credentials),"[]\n","application/json"),200)==nlohmann::json::array());
    CHECK(listener.store.starter_items(account->ref)==saved);
    Listener absent(false); httplib::Client no_facade("127.0.0.1",absent.port()); const auto absent_login=login(no_facade);
    CHECK(result(no_facade.Post("/userGameItemsBatch/"+absent_login.guid,headers(absent_login),"[]","application/json"),404).is_null());
    CHECK_FALSE(absent.store.starter_items(absent.store.find_account("76561197960265729")->ref));
}
TEST_CASE("Item batch HTTP rejects invalid bodies identities raw aliases and methods without mutation") {
    Listener listener; httplib::Client client("127.0.0.1",listener.port()); const auto credentials=login(client);
    const auto account=listener.store.find_account("76561197960265729"); REQUIRE(account);
    const auto owned=result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200); REQUIRE(owned.size()==9);
    const auto saved=listener.store.starter_items(account->ref); const auto path="/userGameItemsBatch/"+credentials.guid;
    const auto instance=owned.at(0).at("UserGameItemGuid").get<std::string>(); auto uppercase=instance;
    for(auto& c:uppercase) if(c>='a' && c<='f') c=static_cast<char>(c-'a'+'A');
    for(const auto& body:std::vector<std::string>{"", "{", "{}", "true", "\"text\"", "[null]", "[1]", "[{}]", "[\"\"]",
        R"(["00000000-0000-0000-0000-000000000000"])",R"(["30000000000000000000000000000099"])",
        nlohmann::json::array({instance,uppercase}).dump(),
        nlohmann::json::array({instance+"/"}).dump()})
        CHECK(result(client.Post(path,headers(credentials),body,"application/json"),400).is_null());
    CHECK(result(client.Post(path,headers(credentials),nlohmann::json::array({instance,"30000000-0000-0000-0000-000000000099"}).dump(),"application/json"),404).is_null());
    nlohmann::json too_many=nlohmann::json::array(); for(int i=0;i<257;++i) too_many.push_back(instance);
    CHECK(result(client.Post(path,headers(credentials),too_many.dump(),"application/json"),413).is_null()); // byte budget wins before ID count
    // Exactly 8192 bytes remain a bounded empty read, never an enumerate-all request.
    const auto exact=std::string("[]")+std::string(8190,' ');
    CHECK(result(client.Post(path,headers(credentials),exact,"application/json"),200)==nlohmann::json::array());
    CHECK(result(client.Post(path,headers(credentials),chunks(exact),"application/json"),200)==nlohmann::json::array());
    CHECK(result(client.Post(path,headers(credentials),exact+" ","application/json"),413).is_null());
    CHECK(result(client.Post(path,headers(credentials),chunks(exact+" "),"application/json"),413).is_null());
    CHECK(result(client.Post(path,headers(credentials),"[]","text/plain"),400).is_null());
    auto duplicate=headers(credentials); duplicate.emplace("Authorization","Basic private-duplicate");
    CHECK(result(client.Post(path,duplicate,"[]","application/json"),400).is_null());
    client.set_path_encode(false);
    for(const auto& alias:std::vector<std::string>{path+"/",path+"?x=1",path+"/extra","/userGameItemsBatch/%30"+credentials.guid.substr(1),
        "/userGameItemsBatch/00000000-0000-0000-0000-000000000000","/userGameItemsBatch/"+std::string(36,'A')})
        CHECK(result(client.Post(alias,headers(credentials),"[]","application/json"),404).is_null());
    for(const auto& response:{client.Get(path,headers(credentials)),client.Put(path,headers(credentials),"[]","application/json"),client.Delete(path,headers(credentials))}) {
        CHECK(result(response,405).is_null()); CHECK(response->get_header_value("Allow")=="POST");
    }
    CHECK(listener.store.starter_items(account->ref)==saved);
    CHECK(listener.store.owned_item_attributes(account->ref,instance).empty());
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
    CHECK(result(client.Get("/userGameItems/"+credentials.guid,headers(credentials)),200)==owned);
}
TEST_CASE("Item batch HTTP binds authorization and reads saved rows after listener restart") {
    hawken::test::TempDirectory temporary; const auto database=temporary.path()/"accounts.sqlite";
    Credentials old; std::string instance; nlohmann::json expected;
    {
        Listener listener(true,database); httplib::Client client("127.0.0.1",listener.port()); old=login(client);
        auto owned=result(client.Get("/userGameItems/"+old.guid,headers(old)),200); REQUIRE(owned.size()==9);
        instance=owned.at(0).at("UserGameItemGuid").get<std::string>();
        CHECK(result(client.Put(broker(old,instance),headers(old),R"({"Preset.BatchRestart":"saved-before-reopen"})","application/json"),200).is_null());
        owned.at(0)["Preset.BatchRestart"]="saved-before-reopen"; expected=nlohmann::json::array({owned.at(0)});
        CHECK(result(client.Post("/userGameItemsBatch/"+old.guid,headers(old),nlohmann::json::array({instance}).dump(),"application/json"),200)==expected);
    }
    Listener listener(true,database); httplib::Client client("127.0.0.1",listener.port()); const auto fresh=login(client);
    CHECK(fresh.guid==old.guid); CHECK(fresh.grant!=old.grant);
    const auto path="/userGameItemsBatch/"+fresh.guid; const auto body=nlohmann::json::array({instance}).dump();
    CHECK(result(client.Post(path,body,"application/json"),401).is_null());
    CHECK(result(client.Post(path,headers(old),body,"application/json"),401).is_null());
    CHECK(result(client.Post("/userGameItemsBatch/30000000-0000-0000-0000-000000000098",headers(fresh),body,"application/json"),401).is_null());
    CHECK(result(client.Post(path,headers(fresh),body,"application/json"),200)==expected);
    const auto account=listener.store.find_account("76561197960265729"); REQUIRE(account);
    const auto saved=listener.store.starter_items(account->ref);
    listener.verifier.active=false;
    CHECK(result(client.Post(path,headers(fresh),body,"application/json"),401).is_null());
    CHECK(listener.store.starter_items(account->ref)==saved);
    CHECK(listener.store.owned_item_attributes(account->ref,instance)==hawken::store::ItemAttributes{{"Preset.BatchRestart","saved-before-reopen"}});
    CHECK(listener.store.wallet(account->ref)==hawken::store::WalletBalances{});
}
