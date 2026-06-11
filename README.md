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

Needs Windows with **MSVC + the Windows SDK** (Visual Studio 2022 or newer, or
the Build Tools) and **CMake 3.16+**. No third-party dependencies: `winhttp`,
`bcrypt`, and `crypt32` ship with the SDK; the MSVC runtime is linked statically.

```powershell
cmake -S . -B build -A x64          # auto-detects the installed Visual Studio
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

## Validating connection reuse (TIME_WAIT)

The whole point of the tool is that the N connections stay open and get reused,
not reopened per request. There are two checks, in increasing order of rigor; run
either from a second PowerShell window while a run is in flight.

### Quick check: ESTABLISHED + TIME_WAIT counts

```powershell
$procId=(Get-Process eh-loadtest).Id; while($true){ "{0}  ESTABLISHED={1}  TIME_WAIT={2}" -f (Get-Date -Format HH:mm:ss),@(Get-NetTCPConnection -OwningProcess $procId -RemotePort 443 -State Established -EA SilentlyContinue).Count,@(Get-NetTCPConnection -RemotePort 443 -State TimeWait -EA SilentlyContinue).Count; Start-Sleep 1 }
```

`ESTABLISHED` should sit flat at ~`--connections`: those are the live keep-alive
sockets. `TIME_WAIT` is the meter for churn.

`TIME_WAIT` is a normal TCP state. The side that *closes* a connection keeps the
local port reserved for ~2 minutes afterwards, so a lost final ACK can still be
answered and stray packets from the old connection drain out before that port is
reused. A reused keep-alive connection is never closed, so it never enters
`TIME_WAIT`. That makes the counter a read on connection reuse:

- **Flat, small `TIME_WAIT`** (a steady plateau) = healthy. Connections are
  long-lived; the trickle is just occasional turnover (the gateway/load balancer
  aging out idle sockets, or the odd failed request reconnecting). A steady
  population of `T` implies roughly `T / 120s` closes per second across the whole
  fleet, i.e. per-connection lifetimes of minutes. Note the plateau builds up
  over the first ~2 minutes as closures accumulate faster than they expire; a
  rise to a level that then holds flat is reaching steady state, not worsening.
- **`TIME_WAIT` climbing with no plateau** = churn. Connections are opened, used
  once, and closed, each paying a fresh TCP+TLS handshake. Left unchecked this
  exhausts the ~16K Windows ephemeral ports (49152-65535) and new connects start
  failing.

To attribute the turnover, read it against the tool's own report line:
`failed=0` with a flat plateau means the closes are server-side recycling; a
rising `failed=` / `err{}` count means requests are erroring and reconnecting
(`fail_request` closes the socket, then the worker backs off and reconnects).

### Stronger check: do the same sockets persist?

The count alone has a blind spot. `ESTABLISHED` can hold at 100 even under full
churn (100 sockets closing and 100 opening each second keeps the count pinned).
And `TIME_WAIT` only accrues on the side that *actively* closes, so churn where
the **server** closes first (the `TIME_WAIT` lands on the gateway, not you) or
where connections die by **RST** (no `TIME_WAIT` at all) is invisible to the
quick check. Since the most likely churn here, the gateway aging out idle
connections, is server-initiated, close the gap by checking connection
*identity*: capture the local ephemeral ports each tick and diff them.

```powershell
$procId = (Get-Process eh-loadtest).Id
$prev = @{}
while ($true) {
  $cur = @{}
  Get-NetTCPConnection -OwningProcess $procId -RemotePort 443 -State Established -EA SilentlyContinue |
    ForEach-Object { $cur[$_.LocalPort] = $true }
  $tw   = @(Get-NetTCPConnection -RemotePort 443 -State TimeWait -EA SilentlyContinue).Count
  $held = @($cur.Keys | Where-Object { $prev.ContainsKey($_) }).Count
  $new  = $cur.Count  - $held     # ports that appeared since last tick (creation rate)
  $gone = $prev.Count - $held     # ports that vanished since last tick (close rate)
  "{0}  EST={1}  TW={2}  held={3}  new={4}  gone={5}" -f `
    (Get-Date -Format HH:mm:ss), $cur.Count, $tw, $held, $new, $gone
  $prev = $cur
  Start-Sleep 1
}
```

`held` is how many of this tick's connections are the *same sockets* as last
tick. Keep-alive holds the four-tuple, so `held` tracks `EST` every tick; churn
hands out fresh ephemeral ports, so `held` collapses even while `EST` stays at
100. `new` / `gone` are the per-second create / close rates (both ~0 in steady
state). `held` is the signal to trust: it catches the server-initiated and RST
churn that `TIME_WAIT` misses, and high overlap cannot be faked (`new`/`gone` can
undercount sub-second churn that the 1s sample skips; `held` cannot). Ignore the
first line, where `held=0` because `$prev` starts empty.

The same check as a one-liner:

```powershell
$procId=(Get-Process eh-loadtest).Id; $prev=@{}; while($true){ $cur=@{}; Get-NetTCPConnection -OwningProcess $procId -RemotePort 443 -State Established -EA SilentlyContinue|%{$cur[$_.LocalPort]=$true}; $tw=@(Get-NetTCPConnection -RemotePort 443 -State TimeWait -EA SilentlyContinue).Count; $held=@($cur.Keys|?{$prev.ContainsKey($_)}).Count; "{0}  EST={1}  TW={2}  held={3}  new={4}  gone={5}" -f (Get-Date -Format HH:mm:ss),$cur.Count,$tw,$held,($cur.Count-$held),($prev.Count-$held); $prev=$cur; Start-Sleep 1 }
```

## Container

A multi-stage **Windows** [`Dockerfile`](Dockerfile) builds the binary (servercore
+ VS Build Tools) and ships a self-contained exe (static CRT, only OS DLLs
needed). Match the base image to your host's Windows build for process isolation.

```powershell
docker build --build-arg WINDOWS_VERSION=ltsc2022 -t eh-loadtest:1.0 .
docker run --rm -e EH_CONNECTION_STRING="Endpoint=sb://...;EntityPath=myeventhub" `
  eh-loadtest:1.0 --connections 2000 --interval-ms 1000 --ramp-s 60
```

## Scale to ~40K connections

Run many small instances rather than one large process (single-NIC limits,
per-process handle pressure): `replicas × --connections = total`, e.g.
`20 × 2000 = 40,000`. [`k8s/deployment.yaml`](k8s/deployment.yaml) deploys the
Windows container onto a Windows node pool (`nodeSelector: kubernetes.io/os:
windows`), connection string from a k8s Secret.

## Configuration

Every knob is a CLI flag; the ones carrying secrets or per-environment values
also read an env var (the flag wins when both are set). Precedence, lowest to
highest: connection string (`--connection-string` / `EH_CONNECTION_STRING`) →
individual env vars → CLI flags. `eh-loadtest --help` prints the same list
inline.

### Target

| Flag (env) | Default | Effect |
|---|---|---|
| `--host <fqdn>` (`EH_HOST`) | _required_ | Target host, e.g. `myns.servicebus.windows.net` |
| `--port <p>` | 443 | TLS port |
| `--target <path>` | EH messages endpoint | Request path + query (set from `EntityPath` when a connection string is used) |
| `--verify-tls` | off | Validate the server certificate (default: ignore cert errors, which suits internal/staging endpoints) |

### Auth (token is minted/parsed once, then reused for every request)

| Flag (env) | Default | Effect |
|---|---|---|
| `--connection-string <cs>` (`EH_CONNECTION_STRING`) | _none_ | EH connection string; sets host, key, and (via `EntityPath`) the target + signing URI |
| `--sas-token <value>` (`EH_SAS_TOKEN`) | _none_ | Full `SharedAccessSignature sr=...` header, reused verbatim |
| `--sas-key-name <name>` (`EH_SAS_KEY_NAME`) | _none_ | SAS policy name; with `--sas-key`, mints one token at startup |
| `--sas-key <secret>` (`EH_SAS_KEY`) | _none_ | SAS key (base64) used for minting |
| `--sas-uri <uri>` | `https://<host>/` | Resource URI to sign (entity-scoped when derived from a connection string) |
| `--sas-ttl-s <n>` | 3600 | Minted-token lifetime in seconds |
| `--auth-header <name>` | `Authorization` | Header the token is injected into |

Provide auth one of three ways: a connection string, a ready-made `--sas-token`,
or `--sas-key-name` + `--sas-key` to mint one. Deliver secrets via env (a k8s
Secret, or `op run`) so they never land in a process list or manifest.

### Load model

| Flag (env) | Default | Effect |
|---|---|---|
| `--connections <n>` | 1000 | Sustained connections this process holds (also WinHTTP's per-server pool cap) |
| `--threads <n>` | n/a | Accepted but ignored; WinHTTP owns concurrency via its IOCP pool |
| `--interval-ms <n>` | 1000 | Gap between requests per connection (steady-state lever) |
| `--requests-per-conn <n>` | 0 | Stop a connection after N requests; 0 = unlimited |
| `--ramp-s <n>` | 30 | Spread connection setup over this window |
| `--warmup-s <n>` | 0 | Extra idle hold after ramp before the first reported window (so early turbulence doesn't skew rps) |
| `--duration-s <n>` | 0 | Total run time; 0 = until Ctrl-C |
| `--payload <json>` | small telemetry doc | Request body sent on every POST |

### Resilience / observability

| Flag (env) | Default | Effect |
|---|---|---|
| `--op-timeout-ms <n>` | 15000 | Resolve/connect/send/receive deadline |
| `--reconnect-base-ms <n>` | 500 | Backoff floor after a dropped connection |
| `--reconnect-max-ms <n>` | 10000 | Backoff ceiling (keeps reconnects non-aggressive) |
| `--report-interval-s <n>` | 5 | How often the metrics line prints |

Offered rate per process ≈ `connections / (interval_ms / 1000)` rps once ramped
(2000 conns at 1000ms ≈ 2000 rps).

## Layout

`src/` - `main` · `config` (knobs + CLI/env) · `sas_token` (HMAC-SHA256 via CNG,
minted once) · `metrics` (lock-free counters) · `client` (WinHTTP async engine:
ramp, per-worker state machine, keep-alive, backoff, drain) · `win_util` (UTF-8
to UTF-16). Plus `CMakeLists.txt`, a Windows `Dockerfile`, `k8s/` (Windows-node
Deployment), and `.github/workflows/windows.yml` (MSVC build + container CI).

## License

MIT. See [LICENSE](LICENSE).
