#include "config.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace ehlt {

namespace {

// Pull a value either from "--flag value" or "--flag=value".
struct ArgCursor {
    int argc;
    char** argv;
    int i = 1;

    bool has() const { return i < argc; }
    std::string_view cur() const { return argv[i]; }

    // Given a flag at position i, return its value and advance. Supports the
    // "--flag=value" form inline.
    bool value(std::string_view flag, std::string& out, std::string& err) {
        std::string_view a = cur();
        auto eq = a.find('=');
        if (eq != std::string_view::npos) {
            out = std::string(a.substr(eq + 1));
            return true;
        }
        if (i + 1 >= argc) {
            err = "missing value for " + std::string(flag);
            return false;
        }
        out = argv[++i];
        return true;
    }
};

bool to_u64(const std::string& s, std::uint64_t& out, std::string& err) {
    try {
        out = std::stoull(s);
        return true;
    } catch (...) {
        err = "expected an integer, got '" + s + "'";
        return false;
    }
}

const char* getenv_opt(const char* k) {
    const char* v = std::getenv(k);
    return (v && *v) ? v : nullptr;
}

}  // namespace

bool parse_config(int argc, char** argv, Config& c, bool& show_help, std::string& err) {
    show_help = false;

    // Environment defaults first; CLI flags below can override them. Secrets
    // are best delivered via env (k8s Secret -> env var).
    if (const char* v = getenv_opt("EH_SAS_TOKEN")) c.sas_token = v;
    if (const char* v = getenv_opt("EH_SAS_KEY")) c.sas_key = v;
    if (const char* v = getenv_opt("EH_SAS_KEY_NAME")) c.sas_key_name = v;
    if (const char* v = getenv_opt("EH_HOST")) c.host = v;

    ArgCursor cur{argc, argv};
    for (; cur.has(); ++cur.i) {
        std::string_view a = cur.cur();
        std::string val;
        auto need = [&](const char* flag) { return cur.value(flag, val, err); };
        auto need_u64 = [&](const char* flag, std::uint64_t& dst) {
            return cur.value(flag, val, err) && to_u64(val, dst, err);
        };

        if (a == "-h" || a == "--help") { show_help = true; return true; }
        else if (a.rfind("--host", 0) == 0)            { if (!need("--host")) return false; c.host = val; }
        else if (a.rfind("--port", 0) == 0)            { if (!need("--port")) return false; c.port = val; }
        else if (a.rfind("--target", 0) == 0)          { if (!need("--target")) return false; c.target = val; }
        else if (a == "--verify-tls")                  { c.verify_tls = true; }
        else if (a.rfind("--connections", 0) == 0)     { std::uint64_t t; if (!need_u64("--connections", t)) return false; c.connections = t; }
        else if (a.rfind("--threads", 0) == 0)         { std::uint64_t t; if (!need_u64("--threads", t)) return false; c.threads = t; }
        else if (a.rfind("--requests-per-conn", 0) == 0){ if (!need_u64("--requests-per-conn", c.requests_per_connection)) return false; }
        else if (a.rfind("--interval-ms", 0) == 0)     { if (!need_u64("--interval-ms", c.request_interval_ms)) return false; }
        else if (a.rfind("--duration-s", 0) == 0)      { if (!need_u64("--duration-s", c.duration_s)) return false; }
        else if (a.rfind("--ramp-s", 0) == 0)          { if (!need_u64("--ramp-s", c.ramp_s)) return false; }
        else if (a.rfind("--warmup-s", 0) == 0)        { if (!need_u64("--warmup-s", c.warmup_s)) return false; }
        else if (a.rfind("--payload", 0) == 0)         { if (!need("--payload")) return false; c.payload = val; }
        else if (a.rfind("--sas-token", 0) == 0)       { if (!need("--sas-token")) return false; c.sas_token = val; }
        else if (a.rfind("--sas-uri", 0) == 0)         { if (!need("--sas-uri")) return false; c.sas_uri = val; }
        else if (a.rfind("--sas-key-name", 0) == 0)    { if (!need("--sas-key-name")) return false; c.sas_key_name = val; }
        else if (a.rfind("--sas-key", 0) == 0)         { if (!need("--sas-key")) return false; c.sas_key = val; }
        else if (a.rfind("--sas-ttl-s", 0) == 0)       { if (!need_u64("--sas-ttl-s", c.sas_ttl_s)) return false; }
        else if (a.rfind("--auth-header", 0) == 0)     { if (!need("--auth-header")) return false; c.auth_header = val; }
        else if (a.rfind("--reconnect-base-ms", 0) == 0){ if (!need_u64("--reconnect-base-ms", c.reconnect_base_ms)) return false; }
        else if (a.rfind("--reconnect-max-ms", 0) == 0){ if (!need_u64("--reconnect-max-ms", c.reconnect_max_ms)) return false; }
        else if (a.rfind("--op-timeout-ms", 0) == 0)   { if (!need_u64("--op-timeout-ms", c.op_timeout_ms)) return false; }
        else if (a.rfind("--report-interval-s", 0) == 0){ if (!need_u64("--report-interval-s", c.report_interval_s)) return false; }
        else {
            err = "unknown argument: " + std::string(a);
            return false;
        }
    }

    // Validation.
    if (c.host.empty()) { err = "--host is required (or set EH_HOST)"; return false; }
    if (c.connections == 0) { err = "--connections must be > 0"; return false; }

    const bool can_mint = c.sas_key && c.sas_key_name;
    if (!c.sas_token && !can_mint) {
        err = "auth required: pass --sas-token, or --sas-key-name + --sas-key "
              "(env EH_SAS_TOKEN / EH_SAS_KEY / EH_SAS_KEY_NAME also work)";
        return false;
    }
    return true;
}

std::string describe(const Config& c) {
    std::ostringstream os;
    os << "eventhub-loadtest config\n"
       << "  target            https://" << c.host << ":" << c.port << c.target << "\n"
       << "  verify_tls        " << (c.verify_tls ? "true" : "false") << "\n"
       << "  connections       " << c.connections << " (this process)\n"
       << "  threads           " << c.resolved_threads() << "\n"
       << "  conns/thread       ~" << (c.connections / c.resolved_threads()) << "\n"
       << "  interval_ms       " << c.request_interval_ms << "\n"
       << "  requests/conn     " << (c.requests_per_connection ? std::to_string(c.requests_per_connection) : "unlimited") << "\n"
       << "  ramp_s            " << c.ramp_s << "\n"
       << "  warmup_s          " << c.warmup_s << "\n"
       << "  duration_s        " << (c.duration_s ? std::to_string(c.duration_s) : "until SIGINT") << "\n"
       << "  payload_bytes     " << c.payload.size() << "\n"
       << "  auth              " << (c.sas_token ? "supplied token (reused)" : "minted once from key (reused)") << "\n"
       << "  op_timeout_ms     " << c.op_timeout_ms << "\n"
       << "  reconnect_backoff " << c.reconnect_base_ms << "ms .. " << c.reconnect_max_ms << "ms\n";
    return os.str();
}

void print_usage(const char* argv0) {
    std::cout <<
"Usage: " << argv0 << " --host <fqdn> [auth] [options]\n"
"\n"
"Sustained-keep-alive HTTPS load generator (Azure Event Hubs shaped).\n"
"\n"
"Target:\n"
"  --host <fqdn>             target host, e.g. myns.servicebus.windows.net  (env EH_HOST)\n"
"  --port <p>                default 443\n"
"  --target <path>           request path+query (default Event Hubs messages endpoint)\n"
"  --verify-tls              verify the server certificate (default: off)\n"
"\n"
"Auth (token is generated/parsed ONCE and reused for every request):\n"
"  --sas-token <value>       full 'SharedAccessSignature sr=...' header value  (env EH_SAS_TOKEN)\n"
"  --sas-key-name <name>     SAS policy name, with --sas-key, to mint a token  (env EH_SAS_KEY_NAME)\n"
"  --sas-key <secret>        SAS key (base64)                                  (env EH_SAS_KEY)\n"
"  --sas-uri <uri>           resource URI to sign (default https://<host>/)\n"
"  --sas-ttl-s <n>           minted token lifetime in seconds (default 3600)\n"
"  --auth-header <name>      header to inject the token into (default Authorization)\n"
"\n"
"Load model:\n"
"  --connections <n>         sustained connections this process holds (default 1000)\n"
"  --threads <n>             worker threads / io_contexts (default: CPU count)\n"
"  --interval-ms <n>         gap between requests on a connection (default 1000)\n"
"  --requests-per-conn <n>   stop a connection after N requests (default: unlimited)\n"
"  --ramp-s <n>              spread connection setup over N seconds (default 30)\n"
"  --warmup-s <n>            idle hold at target before steady state (default 0)\n"
"  --duration-s <n>          total run time; 0 = until SIGINT (default 0)\n"
"  --payload <json>          request body (default: small device telemetry doc)\n"
"\n"
"Resilience / observability:\n"
"  --op-timeout-ms <n>       per-request deadline (default 15000)\n"
"  --reconnect-base-ms <n>   backoff floor after a drop (default 500)\n"
"  --reconnect-max-ms <n>    backoff ceiling (default 10000)\n"
"  --report-interval-s <n>   metrics print cadence (default 5)\n"
"  -h, --help\n";
}

}  // namespace ehlt
