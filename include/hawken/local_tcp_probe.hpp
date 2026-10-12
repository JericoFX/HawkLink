#pragma once
#include <cstdint>
#include <memory>

namespace hawken::tcp {
// Local diagnostic TCP establishment only, not an application/game protocol.
// Constructor binds IPv4 127.0.0.1 exclusively and starts one worker. The caller
// supplies the port; port0 selects an isolated fixture port. Startup errors have
// fixed text, never native errors or peer data. No application bytes read/sent.
// The worker closes accepted sockets promptly, with bounded nonblocking accept
// batches and 20ms idle polling. Scheduling latency is not a hard deadline.
// Windows uses SO_EXCLUSIVEADDRUSE; POSIX uses SO_REUSEADDR for TCP TIME_WAIT
// rebind, never SO_REUSEPORT. Immediate post-traffic rebind is not guaranteed.
class LocalTcpProbe {
public:
    explicit LocalTcpProbe(std::uint16_t requested_port);
    ~LocalTcpProbe();
    LocalTcpProbe(const LocalTcpProbe&) = delete;
    LocalTcpProbe& operator=(const LocalTcpProbe&) = delete;
    LocalTcpProbe(LocalTcpProbe&&) = delete;
    LocalTcpProbe& operator=(LocalTcpProbe&&) = delete;

    // Owner-thread stop is idempotent, joins the worker and releases listener/
    // WSA ref. No concurrent stop/destruction, restart or caller callback.
    // No client action is required. running() is a thread-safe health scalar;
    // a fatal worker socket error clears it so the supervisor can fail closed.
    void stop() noexcept;
    bool running() const noexcept;
    // Actual bound port is retained after stop; health does not imply gameplay.
    std::uint16_t port() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
