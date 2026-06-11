// metrics.hpp — lock-free counters for the load generator.
//
// Everything here is an atomic so that thousands of connection coroutines
// running across many threads can bump counters without ever taking a lock.
// The reporter thread only *reads* these atomics; it never coordinates with
// the workers, which keeps the hot path (request send/receive) contention-free.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ehlt {

struct Metrics {
    // Lifetime totals (monotonically increasing).
    std::atomic<std::uint64_t> requests_ok{0};
    std::atomic<std::uint64_t> requests_failed{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_recv{0};

    // Error taxonomy — useful for telling apart "server is shedding load"
    // (HTTP 5xx / 429) from "network is melting" (connect/timeout failures).
    std::atomic<std::uint64_t> err_connect{0};   // TCP/TLS could not be established
    std::atomic<std::uint64_t> err_timeout{0};   // operation deadline exceeded
    std::atomic<std::uint64_t> err_http{0};       // got a response, but status >= 400
    std::atomic<std::uint64_t> err_io{0};         // mid-stream read/write failure

    // Live gauges (go up AND down). Under WinHTTP these count logical workers,
    // not guaranteed open sockets: WinHTTP pools/reuses connections under the
    // hood, so `active` means "workers in a request cycle (incl. keep-alive
    // idle)" and `connecting` means "workers establishing their first send".
    std::atomic<std::int64_t> active_connections{0};   // established + in request loop
    std::atomic<std::int64_t> connecting{0};           // pre-first-response / reconnecting

    // Snapshot used by the reporter to compute per-interval deltas.
    struct Snapshot {
        std::uint64_t requests_ok;
        std::uint64_t requests_failed;
        std::chrono::steady_clock::time_point at;
    };
};

}  // namespace ehlt
