# eventhub-loadtest

A systems-level **C++20 HTTPS load generator** for measuring how an API gateway
behaves under thousands of **sustained, long-lived keep-alive connections**. It
is shaped for the Azure Event Hubs HTTP send endpoint (SAS auth, small JSON
events), but works against any HTTPS endpoint that accepts a small POST body.

It is built for one specific question: *how much CPU does the gateway burn at
steady state, and how much of that is authentication overhead?* To answer it
honestly the client has to behave like a real fleet of well-written producers,
not a benchmark hammer:

- **Steady state, not peak throughput.** Hold N TLS connections open and send a
  small paced request on each; the connection sits idle (keep-alive) between
  sends, exactly like a real producer trickling telemetry.
- **Auth done right.** The SAS token is computed *once* and reused verbatim on
  every request, so what you measure is the *gateway's* validation cost, not the
  client's signing cost.
- **No connection storms.** Connections ramp up gradually and reconnect with
  capped backoff, so transient failures don't turn into a thundering herd that
  would corrupt the very CPU measurement you're taking.

It scales horizontally: each process holds a configurable slice of connections,
and you reach big numbers (e.g. ~40,000) by running many small instances across
nodes/pods rather than one oversized process.

> **Authorized use only.** This generates heavy, sustained traffic. Run it only
> against endpoints you own or are explicitly authorized to test. It is a
> capacity-and-CPU measurement tool, not an attack tool: it paces requests,
> ramps connections gradually, and backs off on failure specifically to avoid
> the connection-storm / DoS behavior you'd want in an attack.

## Why these design choices

| Requirement | Implementation |
|---|---|
| Non-blocking I/O, low contention | Boost.Asio + Boost.Beast, **one `io_context` per thread**, connections pinned to their thread |
| Persistent / reused connections | One TLS handshake per connection, then a request loop over the same socket (`keep_alive(true)`) |
| Sustained, not bursty | Per-connection **pacing timer** (`--interval-ms`) + **gradual ramp** (`--ramp-s`) |
| Token reuse | SAS token parsed/minted **once** at startup, reused verbatim as a header |
| Tunable load | CLI/env knobs for connections, threads, interval, requests/connection |
| Observability | Live metrics line: active conns, rps, error taxonomy |
| Resilient but well-behaved | Capped exponential backoff on reconnect (no reconnect storms) |
| Horizontal scale | Small per-pod connection count × many replicas (see `k8s/`) |

Boost.Beast (not cpprestsdk, which Microsoft has archived) gives header-only
HTTP+TLS on top of Asio; C++20 coroutines turn the connect -> handshake -> loop
state machine into linear, readable code while staying fully async.

## How it works

```
                 main thread
                     |
   spawns one io_context per worker thread (default: CPU count)
                     |
   +-----------------+------------------+   ... pinned, no work-stealing
   |                 |                  |
 thread 0          thread 1           thread N
 io_context        io_context         io_context
   |                 |                  |
 ramp spawner      ramp spawner       ramp spawner   <- staggers connection
   |  \  \           |  \               |               setup over --ramp-s
  conn conn ...     conn conn ...      conn ...        (anti connection-storm)
   |
   +-- supervise_connection()  (capped-backoff reconnect loop)
         |
         +-- run_session()  [coroutine]
               resolve -> TCP connect -> TLS handshake   (once)
               loop:
                 write small JSON POST  (reused token + body)
                 read response, classify status
                 wait --interval-ms     (idle keep-alive)   <- the steady-state lever
               until quota / Connection: close / error

   reporter thread  -- reads lock-free atomics, prints rps + error taxonomy
```

Each connection lives entirely on the thread that created it, so the hot path
(write/read/wait) touches only thread-local state plus a few atomic counters.
That is what lets a single process hold thousands of connections without lock
contention, and what makes adding cores scale close to linearly.

## Build

Dependencies: a C++20 compiler (GCC 10+/Clang 14+), CMake 3.16+, Boost 1.74+
(`system` + Beast headers), OpenSSL.

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential cmake libboost-system-dev libssl-dev
# macOS (Homebrew)
brew install cmake boost openssl@3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# binary: build/eh-loadtest
```

On macOS, point CMake at Homebrew OpenSSL if needed:
`-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.

## Run

Minimal (token supplied directly, reused for every request):

```bash
export EH_SAS_TOKEN='SharedAccessSignature sr=...&sig=...&se=...&skn=...'
./build/eh-loadtest \
  --host myns.servicebus.windows.net \
  --target '/myeventhub/messages?api-version=2014-01' \
  --connections 2000 --interval-ms 1000 --ramp-s 60
```

Let the tool mint the token once from a SAS key:

```bash
export EH_SAS_KEY_NAME='RootManageSharedAccessKey'
export EH_SAS_KEY='<base64-sas-key>'
./build/eh-loadtest --host myns.servicebus.windows.net \
  --target '/myeventhub/messages?api-version=2014-01' \
  --connections 2000 --sas-ttl-s 3600
```

Sample output:

```
[load] starting 8 worker threads, 2000 connections, ramp 60s
[load] active=1340 connecting=12 rps=1280 ok=64031 failed=3 err{connect=1,timeout=0,http=2,io=0} (10s window)
```

`--help` lists every knob.

## Reaching ~40,000 connections

One process is intentionally *not* the way to get to 40K — file-descriptor
ceilings and a single host's NIC make that unrepresentative of real distributed
clients. Scale horizontally:

```
replicas × --connections = total sustained connections
   20     ×     2000      =        40,000
```

See [`k8s/deployment.yaml`](k8s/deployment.yaml). SAS material comes from a
Kubernetes Secret via env vars; nothing secret is baked into the image.

If you *do* push thousands of connections in a single process, raise the fd
limit first: `ulimit -n 65535` (and matching `securityContext`/node config in
k8s).

## Key tuning knobs

| Flag (env) | Default | Effect |
|---|---|---|
| `--connections` | 1000 | Sustained connections this process holds |
| `--threads` | CPU count | Worker threads = independent `io_context`s |
| `--interval-ms` | 1000 | Gap between requests on each connection (the steady-state lever) |
| `--requests-per-conn` | 0 (∞) | Stop a connection after N requests |
| `--ramp-s` | 30 | Spread connection setup over this window (anti-storm) |
| `--warmup-s` | 0 | Idle hold at target before steady state |
| `--duration-s` | 0 (∞) | Total run time; 0 = until SIGINT |
| `--op-timeout-ms` | 15000 | Per-request read/write deadline |
| `--reconnect-base/max-ms` | 500 / 10000 | Capped backoff after a dropped connection |
| `--verify-tls` | off | Verify the server certificate |

Effective offered rate ≈ `connections / (interval_ms / 1000)` requests/sec per
process (once fully ramped). 2000 conns at 1000ms ≈ 2000 rps/pod.

## Project layout

```
src/
  main.cpp        entry point: parse config, run client
  config.{hpp,cpp} all tuning knobs + CLI/env parsing
  sas_token.{hpp,cpp} SAS token mint (HMAC-SHA256, computed once)
  metrics.hpp     lock-free atomic counters + gauges
  client.{hpp,cpp} engine: per-thread io_context, ramp, keep-alive loop, backoff
CMakeLists.txt    Release build
Dockerfile        multi-stage container image
k8s/deployment.yaml  horizontal scale-out to ~40K connections
```

## License

MIT. See [LICENSE](LICENSE).
