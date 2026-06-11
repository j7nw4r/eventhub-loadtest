#include "client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sas_token.hpp"
#include "win_util.hpp"

#pragma comment(lib, "winhttp.lib")

namespace ehlt {

std::string resolve_auth_value(const Config& cfg) {
    if (cfg.sas_token) return *cfg.sas_token;
    // Mint exactly once. We read the wall clock here (the one place we need it)
    // to set the token expiry; everything downstream reuses this string.
    const std::string uri =
        cfg.sas_uri.value_or("https://" + cfg.host + "/");
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    return make_sas_token(uri, *cfg.sas_key_name, *cfg.sas_key, now, cfg.sas_ttl_s);
}

namespace {

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

struct Client::Impl {
    // One logical connection. Under Beast this was a coroutine frame; under
    // WinHTTP async it is an explicit state machine advanced by
    // WinHttpSetStatusCallback completions. A worker churns one request handle
    // per send and lets WinHTTP's own pool keep the underlying TLS socket warm
    // for keep-alive reuse.
    struct Worker {
        Impl* impl = nullptr;
        HINTERNET hRequest = nullptr;
        PTP_TIMER timer = nullptr;       // pacing + backoff (thread-pool timer)
        // Serializes state transitions across WinHTTP pool threads + the timer
        // callback. Recursive as a belt-and-suspenders guard: WinHttpCloseHandle
        // is called while held and HANDLE_CLOSING re-enters; WinHTTP delivers
        // that callback asynchronously (off this stack), but recursion costs
        // nothing and removes any same-thread re-entry deadlock.
        std::recursive_mutex mtx;
        std::vector<char> rbuf;          // body drain scratch (valid until READ_COMPLETE)
        std::uint64_t sent = 0;          // requests completed this session (quota)
        std::uint64_t backoff_ms = 0;    // next reconnect wait
        bool counted = false;            // contributing to a live gauge
        bool established = false;        // promoted from "connecting" to "active"
        bool closing = false;            // a WinHttpCloseHandle is in flight
        bool started = false;            // launcher has brought this worker online
        bool dead_finalized = false;     // finalize_dead ran (idempotent guard)
        enum class Next { Pace, Backoff, Dead } next = Next::Pace;
    };

    Config cfg;
    Metrics& metrics;
    std::string auth_value;
    std::string payload;             // stable buffer for inline POST body
    std::atomic<bool> stopping{false};

    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    std::wstring wtarget;            // request path+query
    std::wstring wheaders;           // Authorization + Content-Type + User-Agent
    DWORD security_flags = 0;        // cert-error ignore flags (0 when verifying)

    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<std::size_t> live{0};
    HANDLE stop_event = nullptr;     // signaled on Ctrl handler / duration
    HANDLE done_event = nullptr;     // signaled when the last worker dies
    std::thread launcher;

    static Impl* g_self;             // for the console control handler

    Impl(Config c, Metrics& m) : cfg(std::move(c)), metrics(m) {
        auth_value = resolve_auth_value(cfg);
        payload = cfg.payload;
        wtarget = widen(cfg.target);

        // One header block, reused verbatim on every request (Host and
        // Content-Length are added by WinHTTP). The token is the whole point of
        // minting once: we never recompute auth on the hot path.
        std::string headers = cfg.auth_header + ": " + auth_value + "\r\n" +
                              "Content-Type: application/json\r\n" +
                              "User-Agent: eventhub-loadtest/1.0";
        wheaders = widen(headers);

        if (!cfg.verify_tls) {
            // Load tests routinely hit internal/staging endpoints whose certs
            // don't chain to a public root; default to ignoring cert errors.
            security_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                             SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                             SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                             SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        }
    }

    // --- live-connection gauges (the moral equivalent of the old ConnGuard) ---
    // Called only under worker->mtx. `counted`/`established` make every edge
    // idempotent so a drop-then-reconnect can't double-count.
    void gauge_enter(Worker* w) {
        metrics.connecting.fetch_add(1);
        w->counted = true;
        w->established = false;
    }
    void gauge_promote(Worker* w) {
        metrics.connecting.fetch_sub(1);
        metrics.active_connections.fetch_add(1);
        w->established = true;
    }
    void gauge_exit(Worker* w) {
        if (!w->counted) return;
        if (w->established) metrics.active_connections.fetch_sub(1);
        else metrics.connecting.fetch_sub(1);
        w->counted = false;
        w->established = false;
    }

    void count_error(DWORD err) {
        // Our own shutdown close surfaces as a cancel; don't score it.
        if (err == ERROR_WINHTTP_OPERATION_CANCELLED) return;
        if (err == ERROR_WINHTTP_TIMEOUT) {
            metrics.err_timeout.fetch_add(1);
        } else if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
                   err == ERROR_WINHTTP_CANNOT_CONNECT ||
                   err == ERROR_WINHTTP_SECURE_FAILURE) {
            metrics.err_connect.fetch_add(1);
        } else {
            metrics.err_io.fetch_add(1);
        }
        metrics.requests_failed.fetch_add(1);
    }

    // Arm the per-worker thread-pool timer `ms` from now. ms==0 fires ASAP.
    // We always re-enter the next cycle through the timer so the callback stack
    // stays shallow no matter how tight the pacing.
    void arm_timer(Worker* w, std::uint64_t ms) {
        ULONGLONG rel = ms * 10000ULL;  // 100ns units, negative => relative
        LARGE_INTEGER li;
        li.QuadPart = -static_cast<LONGLONG>(rel);
        FILETIME ft;
        ft.dwLowDateTime = li.LowPart;
        ft.dwHighDateTime = static_cast<DWORD>(li.HighPart);
        SetThreadpoolTimer(w->timer, &ft, 0, 0);
    }

    void finalize_dead(Worker* w) {  // under mtx
        if (w->dead_finalized) return;
        w->dead_finalized = true;
        gauge_exit(w);
        if (live.fetch_sub(1) == 1) SetEvent(done_event);
    }

    // After the current request handle is gone (HANDLE_CLOSING) or never
    // existed, decide what this worker does next.
    void dispatch_next(Worker* w) {  // under mtx
        switch (w->next) {
            case Worker::Next::Pace:
                if (stopping.load()) { finalize_dead(w); return; }
                arm_timer(w, cfg.request_interval_ms);
                return;
            case Worker::Next::Backoff:
                if (stopping.load()) { finalize_dead(w); return; }
                arm_timer(w, w->backoff_ms);
                w->backoff_ms = (std::min)(w->backoff_ms * 2, cfg.reconnect_max_ms);
                return;
            case Worker::Next::Dead:
                finalize_dead(w);
                return;
        }
    }

    void close_request(Worker* w) {  // under mtx
        if (w->hRequest && !w->closing) {
            w->closing = true;
            WinHttpCloseHandle(w->hRequest);  // async; HANDLE_CLOSING follows
        } else if (!w->hRequest) {
            dispatch_next(w);                 // no handle to wait on
        }
        // if already closing, the pending HANDLE_CLOSING will dispatch
    }

    // A request failed (async error or a synchronous WinHTTP false return).
    void fail_request(Worker* w, DWORD err) {  // under mtx
        count_error(err);
        gauge_exit(w);
        w->next = stopping.load() ? Worker::Next::Dead : Worker::Next::Backoff;
        close_request(w);
    }

    // Begin one request cycle: open a request handle, set context/security, and
    // fire the send. The body is sent inline so there's no WinHttpWriteData step.
    void start_cycle(Worker* w) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        if (stopping.load()) { finalize_dead(w); return; }
        if (!w->counted) gauge_enter(w);  // cold start of a (re)connect

        HINTERNET hr = WinHttpOpenRequest(
            hConnect, L"POST", wtarget.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hr) {
            count_error(GetLastError());
            gauge_exit(w);
            w->next = stopping.load() ? Worker::Next::Dead : Worker::Next::Backoff;
            dispatch_next(w);
            return;
        }
        w->hRequest = hr;
        w->closing = false;

        DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(w);
        WinHttpSetOption(hr, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof(ctx));
        if (security_flags) {
            DWORD f = security_flags;
            WinHttpSetOption(hr, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof(f));
        }

        BOOL ok = WinHttpSendRequest(
            hr, wheaders.c_str(), static_cast<DWORD>(-1),
            const_cast<char*>(payload.data()),
            static_cast<DWORD>(payload.size()),
            static_cast<DWORD>(payload.size()), ctx);
        if (!ok) fail_request(w, GetLastError());
    }

    void on_send_complete(Worker* w) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        metrics.bytes_sent.fetch_add(payload.size());
        if (!w->established) gauge_promote(w);
        if (!WinHttpReceiveResponse(w->hRequest, nullptr)) {
            fail_request(w, GetLastError());
        }
    }

    void on_headers(Worker* w) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        DWORD code = 0, len = sizeof(code);
        if (!WinHttpQueryHeaders(
                w->hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                WINHTTP_NO_HEADER_INDEX)) {
            fail_request(w, GetLastError());
            return;
        }
        if (code >= 400) {
            metrics.err_http.fetch_add(1);
            metrics.requests_failed.fetch_add(1);
        } else {
            metrics.requests_ok.fetch_add(1);
        }
        // Drain the body so the socket returns clean to WinHTTP's keep-alive pool.
        if (!WinHttpQueryDataAvailable(w->hRequest, nullptr)) {
            fail_request(w, GetLastError());
        }
    }

    void on_data_available(Worker* w, DWORD avail) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        if (avail == 0) { body_complete(w); return; }
        w->rbuf.resize(avail);
        if (!WinHttpReadData(w->hRequest, w->rbuf.data(), avail, nullptr)) {
            fail_request(w, GetLastError());
        }
    }

    void on_read_complete(Worker* w, DWORD nread) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        metrics.bytes_recv.fetch_add(nread);
        if (nread == 0) { body_complete(w); return; }
        if (!WinHttpQueryDataAvailable(w->hRequest, nullptr)) {
            fail_request(w, GetLastError());
        }
    }

    void body_complete(Worker* w) {  // under mtx
        ++w->sent;
        w->backoff_ms = cfg.reconnect_base_ms;  // healthy cycle resets backoff
        const bool quota_done =
            cfg.requests_per_connection && w->sent >= cfg.requests_per_connection;
        w->next = (quota_done || stopping.load()) ? Worker::Next::Dead
                                                  : Worker::Next::Pace;
        close_request(w);  // -> HANDLE_CLOSING -> dispatch_next
    }

    void on_error(Worker* w, DWORD err) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        fail_request(w, err);
    }

    void on_handle_closing(Worker* w) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        w->hRequest = nullptr;
        w->closing = false;
        dispatch_next(w);
    }

    static void CALLBACK status_cb(HINTERNET, DWORD_PTR ctx, DWORD code,
                                   LPVOID info, DWORD len) {
        if (ctx == 0) return;  // session/connect handle notifications
        Worker* w = reinterpret_cast<Worker*>(ctx);
        Impl* self = w->impl;
        switch (code) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                self->on_send_complete(w);
                break;
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                self->on_headers(w);
                break;
            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                self->on_data_available(w, *reinterpret_cast<DWORD*>(info));
                break;
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                self->on_read_complete(w, len);
                break;
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
                auto* r = reinterpret_cast<WINHTTP_ASYNC_RESULT*>(info);
                self->on_error(w, r->dwError);
                break;
            }
            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                self->on_handle_closing(w);
                break;
            default:
                break;
        }
    }

    static void CALLBACK timer_cb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_TIMER) {
        Worker* w = reinterpret_cast<Worker*>(ctx);
        w->impl->start_cycle(w);
    }

    static BOOL WINAPI ctrl_handler(DWORD type) {
        Impl* self = g_self;
        if (!self) return FALSE;
        switch (type) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
                std::cout << "[load] signal received, draining...\n";
                self->stopping.store(true);
                SetEvent(self->stop_event);
                return TRUE;
            default:
                return FALSE;
        }
    }

    // Bring connections online spread across the ramp window so thousands of
    // TLS handshakes don't land in the same instant.
    void launch() {
        const std::uint64_t step_ms =
            cfg.connections > 0 ? (cfg.ramp_s * 1000) / cfg.connections : 0;
        for (std::size_t i = 0; i < cfg.connections; ++i) {
            if (stopping.load()) break;
            workers[i]->started = true;
            live.fetch_add(1);
            start_cycle(workers[i].get());
            if (step_ms) {
                if (WaitForSingleObject(stop_event, static_cast<DWORD>(step_ms)) ==
                    WAIT_OBJECT_0) {
                    break;
                }
            }
        }
    }

    int run();
};

Client::Impl* Client::Impl::g_self = nullptr;

Client::Client(Config cfg, Metrics& metrics)
    : impl_(std::make_unique<Impl>(std::move(cfg), metrics)) {}

Client::~Client() = default;

int Client::Impl::run() {
    g_self = this;
    SetConsoleCtrlHandler(&Impl::ctrl_handler, TRUE);

    stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    done_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    if (cfg.threads != 0) {
        std::cerr << "[load] note: --threads is ignored under WinHTTP "
                     "(the WinHTTP thread pool owns concurrency)\n";
    }

    // --- Session: async, with a connection pool big enough to approximate N ---
    hSession = WinHttpOpen(L"eventhub-loadtest/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                           WINHTTP_FLAG_ASYNC);
    if (!hSession) {
        std::cerr << "fatal: WinHttpOpen failed (" << GetLastError() << ")\n";
        return 1;
    }

    // WinHTTP pools/reuses sockets to *minimize* connections; raise the per-server
    // cap so the pool can actually hold ~N at once. 'active' is then a count of
    // logical workers in a request cycle, not a guarantee of N open sockets.
    DWORD maxc = static_cast<DWORD>((std::max<std::size_t>)(cfg.connections, 2));
    WinHttpSetOption(hSession, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxc, sizeof(maxc));
    WinHttpSetOption(hSession, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &maxc, sizeof(maxc));

    // Coarser than Beast's per-op deadline: WinHTTP exposes only these four
    // buckets. Idle keep-alive gaps aren't covered (no request handle is open).
    int t = static_cast<int>(cfg.op_timeout_ms);
    WinHttpSetTimeouts(hSession, t, t, t, t);

    if (WinHttpSetStatusCallback(hSession, &Impl::status_cb,
                                 WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK) {
        std::cerr << "fatal: WinHttpSetStatusCallback failed (" << GetLastError() << ")\n";
        return 1;
    }

    INTERNET_PORT port = static_cast<INTERNET_PORT>(std::stoul(cfg.port));
    hConnect = WinHttpConnect(hSession, widen(cfg.host).c_str(), port, 0);
    if (!hConnect) {
        std::cerr << "fatal: WinHttpConnect failed (" << GetLastError() << ")\n";
        return 1;
    }

    // Pre-create all workers (and their timers) so teardown can always reach
    // them; only the ones the launcher reaches enter the live count.
    workers.reserve(cfg.connections);
    for (std::size_t i = 0; i < cfg.connections; ++i) {
        auto w = std::make_unique<Worker>();
        w->impl = this;
        w->backoff_ms = cfg.reconnect_base_ms;
        w->timer = CreateThreadpoolTimer(&Impl::timer_cb, w.get(), nullptr);
        workers.push_back(std::move(w));
    }

    std::cout << "[load] starting " << cfg.connections << " connections, ramp "
              << cfg.ramp_s << "s (WinHTTP async)\n";

    // +1 launcher reference keeps `live` from hitting 0 mid-ramp.
    live.store(1);
    launcher = std::thread([this] {
        launch();
        if (live.fetch_sub(1) == 1) SetEvent(done_event);
    });

    // Reporter: pure reader of the atomics. Hold off the first window until the
    // ramp (+ warmup) is past so early turbulence doesn't skew the rps figure.
    std::thread reporter([this] {
        const double iv = static_cast<double>(cfg.report_interval_s);
        if (cfg.ramp_s || cfg.warmup_s) {
            DWORD hold = static_cast<DWORD>((cfg.ramp_s + cfg.warmup_s) * 1000);
            if (WaitForSingleObject(stop_event, hold) == WAIT_OBJECT_0) return;
        }
        std::uint64_t last =
            metrics.requests_ok.load() + metrics.requests_failed.load();
        while (!stopping.load()) {
            if (WaitForSingleObject(stop_event,
                    static_cast<DWORD>(cfg.report_interval_s * 1000)) == WAIT_OBJECT_0) {
                break;
            }
            std::uint64_t now =
                metrics.requests_ok.load() + metrics.requests_failed.load();
            double rps = iv > 0 ? (now - last) / iv : 0.0;
            last = now;
            print_report_line(metrics, rps, iv);
        }
    });

    // Block until Ctrl-C/duration (stop_event) or everyone finished (done_event).
    HANDLE waits[2] = {stop_event, done_event};
    DWORD waitms = cfg.duration_s ? static_cast<DWORD>(cfg.duration_s * 1000)
                                  : INFINITE;
    DWORD wr = WaitForMultipleObjects(2, waits, FALSE, waitms);
    if (wr == WAIT_TIMEOUT) {
        std::cout << "[load] duration reached, draining...\n";
    }

    // --- Drain ---------------------------------------------------------------
    stopping.store(true);
    SetEvent(stop_event);
    // Stop future pacing/backoff fires, then cancel in-flight requests or
    // finalize idle workers. The HANDLE_CLOSING handshake (async) is what frees
    // each cycle; per-worker mtx serializes against any racing timer callback.
    for (auto& w : workers) SetThreadpoolTimer(w->timer, nullptr, 0, 0);
    for (auto& w : workers) {
        std::lock_guard<std::recursive_mutex> lk(w->mtx);
        if (!w->started || w->dead_finalized) continue;
        if (w->hRequest && !w->closing) {
            w->closing = true;
            WinHttpCloseHandle(w->hRequest);
        } else if (!w->hRequest) {
            finalize_dead(w.get());
        }
    }
    WaitForSingleObject(done_event, 30000);  // best-effort grace

    // No more callbacks, then tear down handles and timers.
    WinHttpSetStatusCallback(hSession, nullptr, 0, 0);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    for (auto& w : workers) {
        SetThreadpoolTimer(w->timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(w->timer, TRUE);
        CloseThreadpoolTimer(w->timer);
    }

    if (launcher.joinable()) launcher.join();
    if (reporter.joinable()) reporter.join();

    std::cout << "[load] final: ok=" << metrics.requests_ok.load()
              << " failed=" << metrics.requests_failed.load()
              << " bytes_sent=" << metrics.bytes_sent.load()
              << " bytes_recv=" << metrics.bytes_recv.load() << "\n";
    return 0;
}

int Client::run() { return impl_->run(); }

}  // namespace ehlt
