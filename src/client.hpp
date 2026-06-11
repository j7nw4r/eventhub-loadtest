// client.hpp — the load generator engine.
//
// Threading model: one io_context per worker thread, and every connection is
// pinned to the thread that created it (it never migrates). That means the hot
// path — write request, read response, wait — touches only thread-local state
// plus a handful of atomics, so adding cores scales close to linearly instead
// of fighting over a single shared run-queue.
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
