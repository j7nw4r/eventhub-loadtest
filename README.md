# eventhub-loadtest

A C++20 **Windows / WinHTTP** HTTPS load generator for measuring API gateway CPU
under thousands of **sustained, keep-alive connections**. Shaped for the Azure
Event Hubs HTTP send endpoint (SAS auth, small JSON events), but works against
any HTTPS endpoint that accepts a small POST body.

It models a fleet of well-behaved producers, not a throughput benchmark: hold N
connections open, send a small paced POST on each (idle keep-alive between
sends), mint the SAS token **once** and reuse it, and ramp/reconnect gently to
avoid connection storms. Networking is native **WinHTTP in async mode**: WinHTTP
owns its IOCP thread pool, and each logical connection is a small state machine
driven by WinHTTP completion callbacks. Because real producers on Windows use
WinHTTP, this exercises a gateway through the same client stack (its TLS,
connection reuse, and header behavior). Reach big numbers (~40K) by running many
small instances, not one oversized process.

> **Authorized use only.** This generates heavy, sustained traffic. Run it only
> against endpoints you own or are authorized to test.

> **Windows-only.** The networking core is WinHTTP and the SAS-token crypto is
> CNG (bcrypt); there is no Linux/macOS build. (Earlier revisions used
> Boost.Beast/OpenSSL and were cross-platform; that backend has been removed.)

## Build

Needs Windows with **MSVC + the Windows SDK** (Visual Studio 2022 or the Build
Tools) and **CMake 3.16+**. No third-party dependencies: `winhttp`, `bcrypt`, and
`crypt32` ship with the SDK.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# binary: build\Release\eh-loadtest.exe   (--help lists every knob)
```

## Run

Simplest: hand it an Event Hubs connection string. It derives the host, the
request target, and the (entity-scoped) signing URI, and mints the SAS token
once. (Examples use PowerShell.)

**Without a secret manager**, set the string and run:

```powershell
$env:EH_CONNECTION_STRING = 'Endpoint=sb://myns.servicebus.windows.net/;SharedAccessKeyName=SendPolicy;SharedAccessKey=<base64-key>;EntityPath=myeventhub'
build\Release\eh-loadtest.exe --connections 2000 --interval-ms 1000 --ramp-s 60
```

**With 1Password**, keep the string out of your shell history and environment by
storing it as a field and resolving it at launch with the [`op`
CLI](https://developer.1password.com/docs/cli/). `op run` substitutes any
`op://vault/item/field` reference (add a `section/` segment if the field lives
under one):

```powershell
# The reference resolves only inside the wrapped process
$env:EH_CONNECTION_STRING = 'op://Private/EventHub Test/connection string'
op run -- build\Release\eh-loadtest.exe --connections 2000 --interval-ms 1000 --ramp-s 60

# Or keep references in an env file (op run --env-file is the tidier option):
#   'EH_CONNECTION_STRING=op://Private/EventHub Test/connection string' | Out-File eh.env -Encoding ascii
#   op run --env-file=eh.env -- build\Release\eh-loadtest.exe --connections 2000 --ramp-s 60
```

A *namespace*-level connection string has no `EntityPath`, so the tool can sign
requests but not pick a hub; add `--target '/<hub>/messages?api-version=2014-01'`
to point at a specific Event Hub.

Or configure the pieces explicitly (any of these override the connection
string):

```powershell
# A) a key (token minted once)
$env:EH_SAS_KEY_NAME = 'RootManageSharedAccessKey'
$env:EH_SAS_KEY      = '<base64-key>'
# B) a ready-made token (reused verbatim): $env:EH_SAS_TOKEN = 'SharedAccessSignature sr=...'
build\Release\eh-loadtest.exe `
  --host myns.servicebus.windows.net `
  --target '/myeventhub/messages?api-version=2014-01' `
  --connections 2000 --interval-ms 1000 --ramp-s 60
```

Output:

```
[load] active=1340 connecting=12 rps=1280 ok=64031 failed=3 err{connect=1,timeout=0,http=2,io=0} (10s window)
```

`active` counts logical workers currently in a request cycle (including the
keep-alive idle between paced sends), not a guaranteed count of open sockets:
WinHTTP pools and reuses connections beneath the API. `--connections` raises
WinHTTP's per-server connection cap so the pool can actually hold ~N at once.

## Scale to ~40K connections

Run many small instances rather than one large process (single-NIC limits,
per-process handle pressure): `instances × --connections = total`, e.g.
`20 × 2000 = 40,000`. Launch several `eh-loadtest.exe` processes, each with its
own `--connections`, pointed at the same endpoint.

<!-- AIDEV-TODO: Dockerfile and k8s/deployment.yaml still assume Linux and won't
     build/run this Windows binary. They need a Windows-container rework
     (servercore build -> nanoserver runtime, nodeSelector kubernetes.io/os:
     windows) before the container/k8s path works again. -->

## Key knobs

| Flag (env) | Default | Effect |
|---|---|---|
| `--connections` | 1000 | Sustained connections this process holds (also WinHTTP's per-server pool cap) |
| `--threads` | n/a | Accepted but ignored; WinHTTP owns concurrency |
| `--interval-ms` | 1000 | Gap between requests per connection (steady-state lever) |
| `--ramp-s` | 30 | Spread connection setup over this window |
| `--duration-s` | 0 | Run time; 0 = until Ctrl-C |
| `--requests-per-conn` | 0 | Stop a connection after N requests; 0 = unlimited |
| `--op-timeout-ms` | 15000 | Resolve/connect/send/receive deadline |
| `--reconnect-base/max-ms` | 500 / 10000 | Capped backoff after a drop |
| `--verify-tls` | off | Validate the server certificate (default: ignore cert errors) |

Offered rate per process ≈ `connections / (interval_ms / 1000)` rps once ramped
(2000 conns at 1000ms ≈ 2000 rps).

## Layout

`src/` - `main` · `config` (knobs + CLI/env) · `sas_token` (HMAC-SHA256 via CNG,
minted once) · `metrics` (lock-free counters) · `client` (WinHTTP async engine:
ramp, per-worker state machine, keep-alive, backoff, drain) · `win_util` (UTF-8
to UTF-16). Plus `CMakeLists.txt` and `.github/workflows/windows.yml` (MSVC build
CI). `Dockerfile` and `k8s/` are stale (Linux) pending a Windows-container rework.

## License

MIT. See [LICENSE](LICENSE).
