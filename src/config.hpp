// config.hpp — runtime configuration + CLI/env parsing.
//
// All tuning knobs live here. Secrets (SAS token, signing key) can come from
// the environment so they never appear in a process list or a k8s manifest.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace ehlt {

struct Config {
    // --- Target -------------------------------------------------------------
    std::string host;                         // e.g. myns.servicebus.windows.net
    std::string port = "443";                 // HTTPS
    std::string target =                      // request path + query
        "/myeventhub/messages?api-version=2014-01";
    bool verify_tls = false;                  // verify server cert (off by default for load tests)

    // --- Load model ---------------------------------------------------------
    std::size_t connections = 1000;           // sustained connections THIS process holds open
    std::size_t threads = 0;                  // 0 => hardware_concurrency()
    std::uint64_t requests_per_connection = 0;// 0 => unlimited (run until duration)
    std::uint64_t request_interval_ms = 1000; // pacing: gap between requests on one connection
    std::uint64_t duration_s = 0;             // 0 => run until SIGINT

    // --- Ramp-up (avoid connection storms) ---------------------------------
    std::uint64_t ramp_s = 30;                // spread connection establishment over this window
    std::uint64_t warmup_s = 0;               // hold at target, idle, before counting steady state

    // --- Payload ------------------------------------------------------------
    // Default is a small, non-empty JSON doc — enough to make the gateway do
    // real deserialization/serialization work without dominating wire time.
    std::string payload =
        R"({"deviceId":"sim-0001","ts":0,"seq":0,"temp":21.5,"status":"ok"})";

    // --- Auth ---------------------------------------------------------------
    // Either supply a ready-made token (reused verbatim), OR supply the pieces
    // and we mint ONE token at startup and reuse it for every request.
    std::optional<std::string> sas_token;     // full "SharedAccessSignature sr=..." value
    std::optional<std::string> sas_uri;       // resource URI to sign (defaults to https://host/)
    std::optional<std::string> sas_key_name;  // SAS policy name (skn)
    std::optional<std::string> sas_key;       // SAS key (base64 secret)
    std::uint64_t sas_ttl_s = 3600;           // token lifetime when minting
    std::string auth_header = "Authorization";// where to inject the token

    // --- Failure handling ---------------------------------------------------
    std::uint64_t reconnect_base_ms = 500;    // backoff floor after a dropped connection
    std::uint64_t reconnect_max_ms = 10000;   // backoff ceiling (keeps reconnects non-aggressive)
    std::uint64_t op_timeout_ms = 15000;      // per-request read/write deadline

    // --- Observability ------------------------------------------------------
    std::uint64_t report_interval_s = 5;      // how often to print the metrics line

    std::size_t resolved_threads() const {
        if (threads != 0) return threads;
        unsigned hc = std::thread::hardware_concurrency();
        return hc == 0 ? 4 : hc;
    }
};

// Parse argv (and a few env vars) into a Config. Returns false and prints to
// stderr on bad input; sets `show_help` when the user asked for --help.
bool parse_config(int argc, char** argv, Config& out, bool& show_help, std::string& err);

// Human-readable dump of the active configuration (secrets redacted).
std::string describe(const Config& c);

void print_usage(const char* argv0);

}  // namespace ehlt
