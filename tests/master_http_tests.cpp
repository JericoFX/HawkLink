#include "hawken/master_http.hpp"
#include <cpp-httplib/httplib.h>
#include <doctest/doctest.h>
#include <thread>

namespace {
class LocalListener {
public:
    explicit LocalListener(const hawken::master::MasterDirectory& directory) {
        hawken::master::configure_master_http(server_, directory);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 1) throw std::runtime_error("Loopback listener could not bind.");
        worker_ = std::jthread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        if (!server_.is_running()) {
            server_.stop();
            throw std::runtime_error("Loopback listener did not start.");
        }
    }
    ~LocalListener() { server_.stop(); }
    int port() const { return port_; }
private:
    httplib::Server server_;
    int port_ = -1;
    std::jthread worker_;
};

hawken::master::ServerMetadata sample() {
    return {"server-one", "South America", "fixture-build", "A+B%20", "MapOne",
            "Deathmatch", "127.0.0.1", 7777, 12, true, false};
}
}

TEST_CASE("Master HTTP serves recovered listing envelope over an ephemeral loopback port") {
    const hawken::master::MasterDirectory directory({sample()});
    const LocalListener listener(directory);
    httplib::Client client("127.0.0.1", listener.port());
    // The SDK builds an already escaped RFC3986 target. Preserve those wire bytes.
    client.set_path_encode(false);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    const auto response = client.Get("/gameServerListings?Region=South%20America&ServerName=A%2BB%2520");
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->get_header_value("Content-Type").starts_with("application/json"));
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body.at("Status") == 200);
    CHECK(body.at("Message") == "");
    REQUIRE(body.at("Result").size() == 1);
    CHECK(body.at("Result")[0].at("Guid") == "server-one");
    CHECK(body.at("Result")[0].at("ServerName") == "A+B%20");
    CHECK(body.at("Result")[0].at("Users") == nlohmann::json::array());
    CHECK(body.at("Result")[0].at("Port") == "7777");

    const auto by_id = client.Get("/gameServerListings/server-one");
    REQUIRE(by_id);
    CHECK(by_id->status == 200);
    CHECK(nlohmann::json::parse(by_id->body).at("Result").is_object());
}

TEST_CASE("Master HTTP rejects player APIs malformed queries and writes without echoing data") {
    const hawken::master::MasterDirectory directory({sample()});
    const LocalListener listener(directory);
    httplib::Client client("127.0.0.1", listener.port());
    client.set_path_encode(false);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    const auto absent = client.Get("/session");
    REQUIRE(absent);
    CHECK(absent->status == 404);
    CHECK(nlohmann::json::parse(absent->body).at("Result").is_null());
    const auto invalid = client.Get("/gameServerListings?UserId=never-echo-this");
    REQUIRE(invalid);
    CHECK(invalid->status == 400);
    CHECK(nlohmann::json::parse(invalid->body).at("Status") == 400);
    CHECK(invalid->body.find("never-echo-this") == std::string::npos);
    const auto write = client.Post("/gameServerListings", "never-echo-this", "text/plain");
    REQUIRE(write);
    CHECK(write->status == 405);
    CHECK(nlohmann::json::parse(write->body).at("Status") == 405);
    CHECK(write->body.find("never-echo-this") == std::string::npos);
}
