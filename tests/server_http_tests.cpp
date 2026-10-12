#include "hawken/server_http.hpp"
#include <cpp-httplib/httplib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
class LocalListener {
public:
    LocalListener() {
        hawken::server::configure_server_http(server_);
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

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("Raw loopback sockets could not initialize.");
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

class SocketOwner {
public:
    SocketOwner() : socket_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        if (socket_ == invalid_socket)
            throw std::runtime_error("Raw loopback socket could not open.");
    }
    ~SocketOwner() {
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
    }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    NativeSocket get() const { return socket_; }
private:
    NativeSocket socket_;
};

std::string raw_login_without_body(int port, std::size_t declared_length = 8192) {
#ifdef _WIN32
    const SocketRuntime runtime;
#endif
    const SocketOwner socket;
#ifdef _WIN32
    const DWORD timeout = 1000;
    const auto* timeout_bytes = reinterpret_cast<const char*>(&timeout);
#else
    const timeval timeout{1, 0};
    const auto* timeout_bytes = &timeout;
#endif
    if (setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, timeout_bytes,
            static_cast<int>(sizeof(timeout))) != 0
        || setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, timeout_bytes,
            static_cast<int>(sizeof(timeout))) != 0)
        throw std::runtime_error("Raw loopback timeout could not be set.");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) != 0)
        throw std::runtime_error("Raw loopback socket could not connect.");
    // Exactly one declared length and no body bytes; keep the write side open.
    const std::string request = "POST /steam/login HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
        "Content-Length: " + std::to_string(declared_length) + "\r\nConnection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
#ifdef _WIN32
        const auto count = ::send(socket.get(), request.data() + sent,
            static_cast<int>(request.size() - sent), 0);
#else
        const auto count = ::send(socket.get(), request.data() + sent,
            request.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) throw std::runtime_error("Raw loopback request could not be sent.");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(socket.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
        if (count == 0) break; // Require the server to close, rather than wait for a body.
        if (count < 0) throw std::runtime_error("Raw loopback response timed out or failed.");
        const auto size = static_cast<std::size_t>(count);
        if (response.size() + size > 16384)
            throw std::runtime_error("Raw loopback response exceeded the fixture limit.");
        response.append(buffer, size);
    }
    return response;
}

void configure_client(httplib::Client& client) {
    client.set_path_encode(false);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
}

nlohmann::json checked_envelope(const httplib::Response& response, int status) {
    CHECK(response.status == status);
    CHECK(response.get_header_value("Content-Type").starts_with("application/json"));
    const auto body = nlohmann::json::parse(response.body);
    REQUIRE(body.is_object());
    REQUIRE(body.size() == 3);
    CHECK(body.at("Status") == status);
    CHECK(body.at("Message").is_string());
    REQUIRE(body.contains("Result"));
    CHECK_FALSE(body.contains("AccessGrant"));
    CHECK_FALSE(body.contains("UserId"));
    CHECK_FALSE(body.contains("NewUser"));
    return body;
}
}

TEST_CASE("Server HTTP service status is a string in the recovered envelope") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    const auto response = client.Get("/status/services/");
    REQUIRE(response);
    const auto body = checked_envelope(*response, 200);
    CHECK(body.at("Message") == "");
    REQUIRE(body.at("Result").is_string());
    CHECK_FALSE(body.at("Result").get<std::string>().empty());
    CHECK_FALSE(body.contains("IsOnline"));
}

TEST_CASE("Server HTTP client status contains the original initial client version") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    const auto response = client.Get("/status/game_client/");
    REQUIRE(response);
    const auto body = checked_envelope(*response, 200);
    CHECK(body.at("Message") == "");
    REQUIRE(body.at("Result").is_string());
    CHECK(body.at("Result") == "1.6.0.154767");
}

TEST_CASE("Server HTTP version identifies the local facade and reports current UTC") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto response = client.Get("/version");
    const auto after = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    REQUIRE(response);
    const auto body = checked_envelope(*response, 200);
    CHECK(body.at("Message") == "");
    const auto& result = body.at("Result");
    REQUIRE(result.is_object());
    REQUIRE(result.size() == 2);
    CHECK(result.at("Version") == "hawken-local");
    REQUIRE(result.at("Time").is_string());
    const auto time = result.at("Time").get<std::string>();
    std::smatch parts;
    const std::regex utc("^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})Z$");
    REQUIRE(std::regex_match(time, parts, utc));
    const std::chrono::year_month_day date{
        std::chrono::year(std::stoi(parts[1].str())),
        std::chrono::month(static_cast<unsigned>(std::stoi(parts[2].str()))),
        std::chrono::day(static_cast<unsigned>(std::stoi(parts[3].str())))};
    REQUIRE(date.ok());
    const int hour = std::stoi(parts[4].str());
    const int minute = std::stoi(parts[5].str());
    const int second = std::stoi(parts[6].str());
    REQUIRE(hour < 24);
    REQUIRE(minute < 60);
    REQUIRE(second < 60);
    const auto reported = std::chrono::sys_days(date) + std::chrono::hours(hour)
        + std::chrono::minutes(minute) + std::chrono::seconds(second);
    CHECK(reported >= before);
    CHECK(reported <= after);
}

TEST_CASE("Server HTTP known routes reject wrong methods with the exact Allow header") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    struct Case { const char* method; const char* path; const char* allow; };
    const Case cases[] = {
        {"POST", "/status/services/", "GET"},
        {"PUT", "/status/game_client/", "GET"},
        {"PATCH", "/version", "GET"},
        {"DELETE", "/version", "GET"},
        {"OPTIONS", "/version", "GET"},
        {"HEAD", "/status/services/", "GET"},
        {"GET", "/steam/login", "POST"},
        {"HEAD", "/steam/login", "POST"},
        {"PUT", "/steam/login", "POST"}
    };
    for (const auto& item : cases) {
        CAPTURE(item.method);
        CAPTURE(item.path);
        httplib::Request request;
        request.method = item.method;
        request.path = item.path;
        const auto response = client.send(request);
        REQUIRE(response);
        CHECK(response->status == 405);
        CHECK(response->get_header_value("Allow") == item.allow);
        if (request.method == "HEAD") {
            CHECK(response->body.empty());
        } else {
            CHECK(checked_envelope(*response, 405).at("Result").is_null());
        }
    }
}

TEST_CASE("Server HTTP rejects path aliases queries and directory APIs without echoing") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    const char* paths[] = {
        "/status/services", "/status/game_client", "/version/", "/steam/login/",
        "/Status/services/", "/Steam/login", "/api/version", "/api/steam/login",
        "/status//services/", "/status/%73ervices/", "/version?token=never-echo-this",
        "/users/never-echo-this", "/gameServerListings"
    };
    for (const auto* path : paths) {
        CAPTURE(path);
        const auto response = client.Get(path);
        REQUIRE(response);
        CHECK(checked_envelope(*response, 404).at("Result").is_null());
        CHECK_FALSE(response->has_header("Allow"));
        CHECK(response->body.find("never-echo-this") == std::string::npos);
    }
    const auto write = client.Post("/unknown", "never-echo-this", "text/plain");
    REQUIRE(write);
    CHECK(checked_envelope(*write, 404).at("Result").is_null());
    CHECK(write->body.find("never-echo-this") == std::string::npos);
}

TEST_CASE("Server HTTP Steam login is unavailable independent of submitted content") {
    const LocalListener listener;
    httplib::Client client("127.0.0.1", listener.port());
    configure_client(client);
    const char* contents[] = {"", "{invalid-json", "never-echo-this"};
    std::string first_response;
    for (const auto* content : contents) {
        const auto response = client.Post("/steam/login", content, "application/json");
        REQUIRE(response);
        const auto body = checked_envelope(*response, 503);
        CHECK(body.at("Result").is_null());
        CHECK_FALSE(body.at("Message").get<std::string>().empty());
        CHECK(response->get_header_value("Connection") == "close");
        CHECK(response->body.find("never-echo-this") == std::string::npos);
        CHECK(response->body.find("AccessGrant") == std::string::npos);
        CHECK(response->body.find("UserId") == std::string::npos);
        CHECK(response->body.find("NewUser") == std::string::npos);
        if (first_response.empty()) first_response = response->body;
        CHECK(response->body == first_response);
    }
}

TEST_CASE("Server HTTP Steam login responds without waiting for the declared body") {
    const LocalListener listener;
    const auto response = raw_login_without_body(listener.port());
    CHECK(response.starts_with("HTTP/1.1 503 "));
    const auto boundary = response.find("\r\n\r\n");
    REQUIRE(boundary != std::string::npos);
    const auto headers = response.substr(0, boundary);
    CHECK(headers.find("\r\nConnection: close") != std::string::npos);
    CHECK(headers.find("\r\nContent-Type: application/json") != std::string::npos);
    const auto body = nlohmann::json::parse(response.substr(boundary + 4));
    REQUIRE(body.is_object());
    REQUIRE(body.size() == 3);
    CHECK(body.at("Status") == 503);
    REQUIRE(body.at("Message").is_string());
    CHECK_FALSE(body.at("Message").get<std::string>().empty());
    CHECK(body.at("Result").is_null());
    CHECK_FALSE(body.contains("AccessGrant"));
    CHECK_FALSE(body.contains("UserId"));
    CHECK_FALSE(body.contains("NewUser"));
}

TEST_CASE("Server HTTP rejects oversized nonsettings declared body before unavailable login") {
    const LocalListener listener;
    const auto response = raw_login_without_body(listener.port(), 65536);
    CHECK(response.starts_with("HTTP/1.1 413 "));
    const auto boundary = response.find("\r\n\r\n");
    REQUIRE(boundary != std::string::npos);
    CHECK(response.substr(0, boundary).find("\r\nConnection: close") != std::string::npos);
    const auto body = nlohmann::json::parse(response.substr(boundary + 4));
    CHECK(body.at("Status") == 413);
    CHECK(body.at("Result").is_null());
    CHECK_FALSE(body.contains("AccessGrant"));
    CHECK_FALSE(body.contains("UserId"));
}
