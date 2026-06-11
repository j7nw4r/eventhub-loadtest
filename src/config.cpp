#include "config.hpp"

#include <cctype>
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

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

}  // namespace

bool parse_connection_string(const std::string& cs, Config& c, std::string& err) {
    std::string endpoint, key_name, key, entity, token;

    // Split on ';' into key=value pairs. Each value is "everything after the
    // FIRST '='" so base64 keys (which end in '=' padding) survive intact.
    std::size_t pos = 0;
    while (pos < cs.size()) {
        std::size_t semi = cs.find(';', pos);
        std::string_view part =
            std::string_view(cs).substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? cs.size() : semi + 1;

        std::string field = trim(part);
        if (field.empty()) continue;
        auto eq = field.find('=');
        if (eq == std::string::npos) {
            err = "malformed connection string near '" + field + "' (expected key=value)";
            return false;
        }
        std::string k = trim(std::string_view(field).substr(0, eq));
        std::string v = trim(std::string_view(field).substr(eq + 1));

        if (iequals(k, "Endpoint")) endpoint = v;
        else if (iequals(k, "SharedAccessKeyName")) key_name = v;
        else if (iequals(k, "SharedAccessKey")) key = v;
        else if (iequals(k, "EntityPath")) entity = v;
        else if (iequals(k, "SharedAccessSignature")) token = v;
        // Unknown fields (e.g. UseDevelopmentEmulator) are ignored on purpose.
    }

    // Host from Endpoint: strip scheme (sb://, https://, http://) and any
    // trailing slash, leaving "<ns>.servicebus.windows.net".
    if (!endpoint.empty()) {
        std::string_view e = endpoint;
        if (auto s = e.find("://"); s != std::string_view::npos) e = e.substr(s + 3);
        while (!e.empty() && e.back() == '/') e.remove_suffix(1);
        c.host = std::string(e);
    }
    if (!key_name.empty()) c.sas_key_name = key_name;
    if (!key.empty()) c.sas_key = key;
    if (!token.empty()) c.sas_token = token;  // pre-baked token; reused verbatim

    if (!entity.empty()) {
        // Request path for the Event Hubs HTTP send endpoint...
        c.target = "/" + entity + "/messages?api-version=2014-01";
        // ...and the entity-scoped signing URI, so an entity-level SAS policy
        // (not just a namespace-wide one) verifies correctly.
        c.sas_uri = "https://" + c.host + "/" + entity;
    }

    if (c.host.empty()) {
        err = "connection string is missing Endpoint=sb://<host>/";
        return false;
    }
    if (!c.sas_token && !(c.sas_key && c.sas_key_name)) {
        err = "connection string is missing SharedAccessKeyName/SharedAccessKey "
              "(or a SharedAccessSignature token)";
        return false;
    }
    return true;
}

bool parse_config(int argc, char** argv, Config& c, bool& show_help, std::string& err) {
    show_help = false;

    // Layer 0 (lowest precedence): a connection string sets host + auth +
    // entity-derived target/URI in one shot. It can come from the environment
    // or from --connection-string (the flag wins over the env var). Individual
    // env vars and CLI flags below then override anything it set.
    {
        std::string cs;
        if (const char* v = getenv_opt("EH_CONNECTION_STRING")) cs = v;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a == "--connection-string" && i + 1 < argc) cs = argv[i + 1];
            else if (a.rfind("--connection-string=", 0) == 0) cs = std::string(a.substr(20));
        }
        if (!cs.empty() && !parse_connection_string(cs, c, err)) return false;
    }

    // Environment defaults next; CLI flags below can override them. Secrets
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
        else if (a.rfind("--connection-string", 0) == 0) { if (!need("--connection-string")) return false; /* applied in layer 0 */ }
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
       << "  threads           " << (c.threads ? std::to_string(c.threads) + " (ignored; WinHTTP owns concurrency)" : std::string("WinHTTP-managed")) << "\n"
       << "  interval_ms       " << c.request_interval_ms << "\n"
       << "  requests/conn     " << (c.requests_per_connection ? std::to_string(c.requests_per_connection) : "unlimited") << "\n"
       << "  ramp_s            " << c.ramp_s << "\n"
       << "  warmup_s          " << c.warmup_s << "\n"
       << "  duration_s        " << (c.duration_s ? std::to_string(c.duration_s) : "until Ctrl-C") << "\n"
       << "  payload_bytes     " << c.payload.size() << "\n"
       << "  auth              " << (c.sas_token ? "supplied token (reused)" : "minted once from key (reused)") << "\n"
       << "  signing_uri       " << (c.sas_token ? "(n/a, token supplied)" : c.sas_uri.value_or("https://" + c.host + "/")) << "\n"
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
"  --connection-string <cs>  Event Hubs connection string; sets host, key, and\n"
"                            (via EntityPath) the target + signing URI         (env EH_CONNECTION_STRING)\n"
"  --sas-token <value>       full 'SharedAccessSignature sr=...' header value  (env EH_SAS_TOKEN)\n"
"  --sas-key-name <name>     SAS policy name, with --sas-key, to mint a token  (env EH_SAS_KEY_NAME)\n"
"  --sas-key <secret>        SAS key (base64)                                  (env EH_SAS_KEY)\n"
"  --sas-uri <uri>           resource URI to sign (default https://<host>/)\n"
"  --sas-ttl-s <n>           minted token lifetime in seconds (default 3600)\n"
"  --auth-header <name>      header to inject the token into (default Authorization)\n"
"\n"
"Load model:\n"
"  --connections <n>         sustained connections this process holds (default 1000)\n"
"  --threads <n>             accepted but ignored (WinHTTP owns concurrency)\n"
"  --interval-ms <n>         gap between requests on a connection (default 1000)\n"
"  --requests-per-conn <n>   stop a connection after N requests (default: unlimited)\n"
"  --ramp-s <n>              spread connection setup over N seconds (default 30)\n"
"  --warmup-s <n>            hold before the first reported window (default 0)\n"
"  --duration-s <n>          total run time; 0 = until Ctrl-C (default 0)\n"
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
