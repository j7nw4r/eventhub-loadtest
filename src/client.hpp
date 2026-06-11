// client.hpp - the load generator engine (Windows / WinHTTP).
//
// Concurrency model: WinHTTP runs in async mode and owns its own IOCP thread
// pool, so this code does not manage worker threads or event loops. Each logical
// connection is a Worker state machine advanced by WinHttpSetStatusCallback
// completions (send -> receive -> drain -> pace), with per-worker thread-pool
// timers for pacing and backoff. The hot path stays lock-light: only a short
// per-worker mutex guards state transitions, and the metrics are plain atomics.
//
// Windows-only: this replaces the former Boost.Asio/Beast/OpenSSL stack.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "metrics.hpp"

namespace ehlt {

class Client {
public:
    Client(Config cfg, Metrics& metrics);
    ~Client();

    // Blocks until the run finishes (duration elapsed or SIGINT/SIGTERM).
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Resolve the final auth header value once: either the supplied token verbatim,
// or a freshly minted one. Centralized so main() and tests share the logic.
std::string resolve_auth_value(const Config& cfg);

}  // namespace ehlt
