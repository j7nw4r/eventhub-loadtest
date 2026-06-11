# eventhub-loadtest

A C++20 (Boost.Asio/Beast) HTTPS load generator for measuring API gateway CPU
under thousands of **sustained, keep-alive connections**. Shaped for the Azure
Event Hubs HTTP send endpoint (SAS auth, small JSON events), but works against
any HTTPS endpoint that accepts a small POST body.

It models a fleet of well-behaved producers, not a throughput benchmark: hold N
TLS connections open, send a small paced POST on each (idle keep-alive between
sends), mint the SAS token **once** and reuse it, and ramp/reconnect gently to
avoid connection storms. One `io_context` per thread with connections pinned, so
the hot path is lock-free and scales across cores. Reach big numbers (~40K) by
running many small instances, not one oversized process.

> **Authorized use only.** This generates heavy, sustained traffic. Run it only
> against endpoints you own or are authorized to test.

## Build

Needs a C++20 compiler, CMake 3.16+, Boost 1.74+ (Beast headers), OpenSSL.

```bash
# Debian/Ubuntu: apt-get install build-essential cmake libboost-system-dev libssl-dev
# macOS:         brew install cmake boost openssl@3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # macOS: add -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build -j
# binary: build/eh-loadtest   (--help lists every knob)
```

## Run

```bash
# token supplied directly (reused for every request)
export EH_SAS_TOKEN='SharedAccessSignature sr=...&sig=...&se=...&skn=...'
# ...or let the tool mint it once from a key:
# export EH_SAS_KEY_NAME=RootManageSharedAccessKey EH_SAS_KEY=<base64-key>

./build/eh-loadtest \
  --host myns.servicebus.windows.net \
  --target '/myeventhub/messages?api-version=2014-01' \
  --connections 2000 --interval-ms 1000 --ramp-s 60
```

Output:

```
[load] active=1340 connecting=12 rps=1280 ok=64031 failed=3 err{connect=1,timeout=0,http=2,io=0} (10s window)
```

## Scale to ~40K connections

Run many small instances rather than one large process (fd ceilings, single-NIC
limits): `replicas × --connections = total`, e.g. `20 × 2000 = 40,000`. See
[`k8s/deployment.yaml`](k8s/deployment.yaml) (SAS key from a k8s Secret). For
many connections in one process, raise the fd limit first (`ulimit -n 65535`).

## Key knobs

| Flag (env) | Default | Effect |
|---|---|---|
| `--connections` | 1000 | Sustained connections this process holds |
| `--threads` | CPU count | Worker threads = independent `io_context`s |
| `--interval-ms` | 1000 | Gap between requests per connection (steady-state lever) |
| `--ramp-s` | 30 | Spread connection setup over this window |
| `--duration-s` | 0 | Run time; 0 = until SIGINT |
| `--requests-per-conn` | 0 | Stop a connection after N requests; 0 = unlimited |
| `--op-timeout-ms` | 15000 | Per-request read/write deadline |
| `--reconnect-base/max-ms` | 500 / 10000 | Capped backoff after a drop |
| `--verify-tls` | off | Verify the server certificate |

Offered rate per process ≈ `connections / (interval_ms / 1000)` rps once ramped
(2000 conns at 1000ms ≈ 2000 rps).

## Layout

`src/` — `main` · `config` (knobs + CLI/env) · `sas_token` (HMAC-SHA256, minted
once) · `metrics` (lock-free counters) · `client` (per-thread io_context, ramp,
keep-alive loop, backoff). Plus `CMakeLists.txt`, `Dockerfile`, `k8s/`.

## License

MIT. See [LICENSE](LICENSE).
