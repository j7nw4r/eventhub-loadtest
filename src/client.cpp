#include "client.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <thread>

#include "sas_token.hpp"

namespace ehlt {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using namespace std::chrono_literals;

std::string resolve_auth_value(const Config& cfg) {
    if (cfg.sas_token) return *cfg.sas_token;
    // Mint exactly once. We read the wall clock here (the one place we need it)
    // to set the token expiry; everything downstream reuses this string.
    const std::string uri =
        cfg.sas_uri.value_or("https://" + cfg.host + "/");
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    return make_sas_token(uri, *cfg.sas_key_name, *cfg.sas_key, now, cfg.sas_ttl_s);
}

struct Client::Impl {
    Config cfg;
    Metrics& metrics;
    std::string auth_value;
    std::atomic<bool> stopping{false};

    ssl::context ssl_ctx{ssl::context::tls_client};

    // One io_context per worker thread; connections are pinned to whichever
    // context spawned them.
    std::vector<std::unique_ptr<asio::io_context>> contexts;
    std::vector<std::thread> threads;

    Impl(Config c, Metrics& m) : cfg(std::move(c)), metrics(m) {
        auth_value = resolve_auth_value(cfg);

        ssl_ctx.set_default_verify_paths();
        if (cfg.verify_tls) {
            ssl_ctx.set_verify_mode(ssl::verify_peer);
        } else {
            // Load tests routinely hit internal/staging endpoints with certs
            // that won't chain to a public root; default to not verifying.
            ssl_ctx.set_verify_mode(ssl::verify_none);
        }
    }

    // RAII for the live-connection gauges so they stay correct no matter how a
    // coroutine unwinds (clean return, exception, or cancellation).
    struct ConnGuard {
        Metrics& m;
        bool established_ = false;
        explicit ConnGuard(Metrics& mm) : m(mm) { m.connecting.fetch_add(1); }
        void established() {
            m.connecting.fetch_sub(1);
            m.active_connections.fetch_add(1);
            established_ = true;
        }
        ~ConnGuard() {
            if (established_) m.active_connections.fetch_sub(1);
            else m.connecting.fetch_sub(1);
        }
    };

    // Run a single keep-alive session: connect once, then issue paced requests
    // over the SAME connection until the quota is met, the peer closes, or an
    // error is thrown. Returns true if this connection satisfied its request
    // quota (so the supervisor should stop instead of reconnecting).
    asio::awaitable<bool> run_session() {
        auto ex = co_await asio::this_coro::executor;
        ConnGuard guard(metrics);

        ssl::stream<beast::tcp_stream> stream(ex, ssl_ctx);

        // SNI is mandatory for most TLS front ends (incl. Azure) to route to
        // the right vhost and present the right cert.
        if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg.host.c_str())) {
            metrics.err_connect.fetch_add(1);
            throw beast::system_error{
                beast::error_code{static_cast<int>(::ERR_get_error()),
                                  asio::error::get_ssl_category()}};
        }

        auto& lowest = beast::get_lowest_layer(stream);
        const auto op_to = std::chrono::milliseconds(cfg.op_timeout_ms);

        // --- Establish (resolve -> TCP -> TLS) ------------------------------
        try {
            tcp::resolver resolver(ex);
            auto endpoints = co_await resolver.async_resolve(
                cfg.host, cfg.port, asio::use_awaitable);

            lowest.expires_after(op_to);
            co_await lowest.async_connect(endpoints, asio::use_awaitable);

            lowest.expires_after(op_to);
            co_await stream.async_handshake(ssl::stream_base::client,
                                            asio::use_awaitable);
        } catch (const beast::system_error& e) {
            if (e.code() == beast::error::timeout) metrics.err_timeout.fetch_add(1);
            else metrics.err_connect.fetch_add(1);
            throw;
        }
        guard.established();

        // Prebuild the request once. Token + body are reused verbatim on every
        // iteration — minting per request is exactly the client-side overhead
        // we want to avoid (and is not what a sane producer does).
        http::request<http::string_body> req{http::verb::post, cfg.target, 11};
        req.set(http::field::host, cfg.host);
        req.set(cfg.auth_header, auth_value);
        req.set(http::field::content_type, "application/json");
        req.set(http::field::user_agent, "eventhub-loadtest/1.0");
        req.keep_alive(true);
        req.body() = cfg.payload;
        req.prepare_payload();

        asio::steady_timer pacer(ex);
        std::uint64_t sent = 0;

        for (;;) {
            if (stopping.load(std::memory_order_relaxed)) co_return false;

            // --- One request/response round-trip ----------------------------
            try {
                lowest.expires_after(op_to);
                std::size_t nw = co_await http::async_write(stream, req,
                                                            asio::use_awaitable);
                metrics.bytes_sent.fetch_add(nw);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                lowest.expires_after(op_to);
                std::size_t nr = co_await http::async_read(stream, buffer, res,
                                                           asio::use_awaitable);
                metrics.bytes_recv.fetch_add(nr);

                if (res.result_int() >= 400) {
                    metrics.err_http.fetch_add(1);
                    metrics.requests_failed.fetch_add(1);
                } else {
                    metrics.requests_ok.fetch_add(1);
                }

                ++sent;
                if (cfg.requests_per_connection &&
                    sent >= cfg.requests_per_connection) {
                    co_return true;  // quota met — supervisor stops cleanly
                }

                // Respect an explicit Connection: close from the server; the
                // socket is no longer reusable, so reconnect (not an error).
                if (!res.keep_alive()) co_return false;
            } catch (const beast::system_error& e) {
                if (e.code() == beast::error::timeout) metrics.err_timeout.fetch_add(1);
                else metrics.err_io.fetch_add(1);
                metrics.requests_failed.fetch_add(1);
                throw;  // supervisor handles backoff + reconnect
            }

            // --- Pace ----------------------------------------------------------
            // The gap between requests is what makes this a *sustained* model
            // rather than a throughput benchmark. The connection sits idle
            // (keep-alive) between sends, just like a real steady producer.
            if (cfg.request_interval_ms) {
                lowest.expires_never();  // don't let the op-timeout fire while idle
                pacer.expires_after(std::chrono::milliseconds(cfg.request_interval_ms));
                co_await pacer.async_wait(asio::use_awaitable);
            }
        }
    }

    // Reconnect loop with capped exponential backoff. The cap is what keeps a
    // failing endpoint from turning into a reconnect storm that would distort
    // the very CPU measurement we're trying to take.
    asio::awaitable<void> supervise_connection() {
        auto ex = co_await asio::this_coro::executor;
        std::uint64_t backoff = cfg.reconnect_base_ms;

        while (!stopping.load(std::memory_order_relaxed)) {
            bool quota_done = false;
            try {
                quota_done = co_await run_session();
                backoff = cfg.reconnect_base_ms;  // healthy session resets backoff
            } catch (const std::exception&) {
                // Already classified/counted inside run_session.
            }
            if (quota_done || stopping.load(std::memory_order_relaxed)) break;

            asio::steady_timer t(ex);
            t.expires_after(std::chrono::milliseconds(backoff));
            try {
                co_await t.async_wait(asio::use_awaitable);
            } catch (...) { break; }
            backoff = std::min<std::uint64_t>(backoff * 2, cfg.reconnect_max_ms);
        }
        co_return;
    }

    // Per-thread spawner: brings `count` connections online spread across the
    // ramp window, so 1000s of TLS handshakes don't land in the same instant.
    asio::awaitable<void> spawn_ramp(std::size_t count) {
        auto ex = co_await asio::this_coro::executor;
        const std::uint64_t step_ms =
            count > 0 ? (cfg.ramp_s * 1000) / count : 0;

        asio::steady_timer t(ex);
        for (std::size_t i = 0; i < count; ++i) {
            if (stopping.load(std::memory_order_relaxed)) break;
            asio::co_spawn(ex, supervise_connection(), asio::detached);
            if (step_ms) {
                t.expires_after(std::chrono::milliseconds(step_ms));
                try { co_await t.async_wait(asio::use_awaitable); }
                catch (...) { break; }
            }
        }
        co_return;
    }
};

Client::Client(Config cfg, Metrics& metrics)
    : impl_(std::make_unique<Impl>(std::move(cfg), metrics)) {}

Client::~Client() = default;

namespace {

// Distribute N connections across T threads as evenly as possible.
std::vector<std::size_t> split(std::size_t total, std::size_t buckets) {
    std::vector<std::size_t> out(buckets, total / buckets);
    for (std::size_t i = 0; i < total % buckets; ++i) ++out[i];
    return out;
}

void print_report_line(const Metrics& m, double rps, double interval_s) {
    std::cout
        << "[load] active=" << m.active_connections.load()
        << " connecting=" << m.connecting.load()
        << " rps=" << static_cast<std::uint64_t>(rps + 0.5)
        << " ok=" << m.requests_ok.load()
        << " failed=" << m.requests_failed.load()
        << " err{connect=" << m.err_connect.load()
        << ",timeout=" << m.err_timeout.load()
        << ",http=" << m.err_http.load()
        << ",io=" << m.err_io.load() << "}"
        << " (" << interval_s << "s window)\n";
    std::cout.flush();
}

}  // namespace

int Client::run() {
    auto& I = *impl_;
    const std::size_t nthreads = I.cfg.resolved_threads();
    const auto buckets = split(I.cfg.connections, nthreads);

    // Build one io_context per thread and spawn that thread's share of
    // connections through the ramp spawner.
    I.contexts.reserve(nthreads);
    for (std::size_t t = 0; t < nthreads; ++t) {
        I.contexts.push_back(std::make_unique<asio::io_context>(1));
    }

    // SIGINT/SIGTERM -> flip the stop flag and stop every context.
    asio::signal_set signals(*I.contexts[0], SIGINT, SIGTERM);
    signals.async_wait([&I](const boost::system::error_code&, int sig) {
        std::cout << "[load] signal " << sig << " received, draining...\n";
        I.stopping.store(true);
        for (auto& ctx : I.contexts) ctx->stop();
    });

    // Optional hard duration limit.
    asio::steady_timer duration_timer(*I.contexts[0]);
    if (I.cfg.duration_s) {
        duration_timer.expires_after(std::chrono::seconds(I.cfg.duration_s));
        duration_timer.async_wait([&I](const boost::system::error_code& ec) {
            if (ec) return;  // cancelled
            std::cout << "[load] duration reached, draining...\n";
            I.stopping.store(true);
            for (auto& ctx : I.contexts) ctx->stop();
        });
    }

    for (std::size_t t = 0; t < nthreads; ++t) {
        asio::co_spawn(*I.contexts[t], I.spawn_ramp(buckets[t]), asio::detached);
    }

    std::cout << "[load] starting " << nthreads << " worker threads, "
              << I.cfg.connections << " connections, ramp "
              << I.cfg.ramp_s << "s\n";

    // Launch workers. Each thread runs its own context until stopped.
    for (std::size_t t = 0; t < nthreads; ++t) {
        I.threads.emplace_back([ctx = I.contexts[t].get()] {
            // work_guard keeps run() alive even when momentarily idle (e.g.
            // every connection is in its pacing wait).
            auto guard = asio::make_work_guard(*ctx);
            ctx->run();
        });
    }

    // Reporter thread: pure reader of the atomics, computes per-window rps.
    std::thread reporter([&I, nthreads] {
        const double iv = static_cast<double>(I.cfg.report_interval_s);
        std::uint64_t last = I.metrics.requests_ok.load() + I.metrics.requests_failed.load();
        while (!I.stopping.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(I.cfg.report_interval_s));
            if (I.stopping.load()) break;
            std::uint64_t now = I.metrics.requests_ok.load() + I.metrics.requests_failed.load();
            double rps = iv > 0 ? (now - last) / iv : 0.0;
            last = now;
            print_report_line(I.metrics, rps, iv);
        }
    });

    for (auto& th : I.threads) th.join();

    // Reporter may be mid-sleep; flip + nudge it.
    I.stopping.store(true);
    if (reporter.joinable()) reporter.join();

    std::cout << "[load] final: ok=" << I.metrics.requests_ok.load()
              << " failed=" << I.metrics.requests_failed.load()
              << " bytes_sent=" << I.metrics.bytes_sent.load()
              << " bytes_recv=" << I.metrics.bytes_recv.load() << "\n";
    return 0;
}

}  // namespace ehlt
