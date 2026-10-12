#include "hawken/master_http.hpp"
#include "hawken/http_diagnostics.hpp"
#include <cpp-httplib/httplib.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <syncstream>
#include <thread>

namespace {
std::atomic<bool> shutdown_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
    "Signal handlers require a lock-free shutdown flag.");

void request_shutdown(int) noexcept {
    shutdown_requested.store(true, std::memory_order_relaxed);
}

void print_help() {
    std::cout << "Usage: hawken-master [--port 1..65535] | --help\n"
        "Local directory-only listener on 127.0.0.1; default port 8080.\n";
}

int serve(int port) {
    const hawken::master::MasterDirectory directory({});
    httplib::Server server;
    hawken::master::configure_master_http(server, directory);
    server.set_logger([](const httplib::Request& request, const httplib::Response& response) {
        const auto diagnostic = hawken::http::classify_request(request.method, request.path);
        std::osyncstream(std::cout) << "http status=" << response.status
            << " category=" << diagnostic.category << " method=" << diagnostic.method
            << '\n' << std::flush;
    });
    server.set_start_handler([port] {
        std::cout << "hawken-master directory-only http://127.0.0.1:"
            << port << " (empty catalog)\n" << std::flush;
    });
    if (!server.bind_to_port("127.0.0.1", port)) {
        std::cerr << "Local listener could not bind.\n";
        return 1;
    }

    try {
        std::atomic<bool> listener_finished{false};
        std::jthread supervisor([&](std::stop_token stop) {
            while (!stop.stop_requested()
                && !listener_finished.load(std::memory_order_relaxed)) {
                // A signal between bind and listen must not be lost. Wait for
                // the accept loop to start before requesting its shutdown.
                if (shutdown_requested.load(std::memory_order_relaxed)
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
        if (!listened) {
            std::cerr << "Local listener could not run.\n";
            return 1;
        }
        std::cout << "Local directory listener stopped.\n";
        return 0;
    } catch (...) {
        server.stop();
        throw;
    }
}
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help();
        return 0;
    }
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--port") {
            std::cerr << "Invalid arguments. Use --help.\n";
            return 2;
        }
        const std::string_view value(argv[2]);
        const auto result = std::from_chars(value.data(), value.data() + value.size(), port, 10);
        if (value.empty()
            || result.ec != std::errc{} || result.ptr != value.data() + value.size()
            || port < 1 || port > 65535) {
            std::cerr << "Invalid port. Expected decimal 1..65535.\n";
            return 2;
        }
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
        return serve(port);
    } catch (...) {
        std::cerr << "Local directory listener failed.\n";
        return 1;
    }
}
