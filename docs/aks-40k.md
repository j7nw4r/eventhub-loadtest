# Running ~40,000 connections on AKS

A runbook for deploying eventhub-loadtest on Azure Kubernetes Service to hold
~40,000 sustained keep-alive connections against a single endpoint. It pairs
with `scripts/aks-40k-setup.sh` (greenfield provisioning), `k8s/deployment.yaml`
(the workload), and the ACR push step in `.github/workflows/windows.yml`.

> Authorized use only. This generates heavy, sustained traffic. Point it only at
> endpoints you own or are authorized to test.

## The one constraint that actually decides this: outbound SNAT

Replica count is the easy part (`replicas x --connections = total`, so
`20 x 2000 = 40,000`). The hard part is that all 40K sockets target **one**
destination, the Event Hubs host on port 443. Every one of those flows needs a
distinct source (SNAT) port on the egress path, and the egress choice sets the
ceiling:

| Egress path | SNAT ports to a single destination | Verdict |
|---|---|---|
| Standard Load Balancer (AKS default) | small per-node budget, pre-allocated | exhausts well before 40K; wrong tool |
| NAT gateway | 64,512 per public IP, up to 16 IPs, drawn from a shared pool on demand | 1 IP just covers 40K; 2 IPs give headroom |

So the cluster routes egress through a **managed NAT gateway with 2 public IPs**
(~129K ports). If outbound stayed on the load balancer, the run would collapse
into exactly the connection churn this tool is built to detect: ports exhaust,
new connects fail, sockets reopen per request.

Two Windows-specific details fall out of that choice:

- **Disable Windows OutboundNAT** on the Windows node pool
  (`--disable-windows-outbound-nat`). Windows enables OutboundNAT by default and
  does its own pod->node port translation; stacked on top of the NAT gateway
  that is a second, redundant port-exhaustion source.
- Disabling Windows OutboundNAT requires the cluster outbound type to be **NAT
  gateway (or UDR), not LoadBalancer**. The setup script sets
  `--outbound-type managedNATGateway`, which satisfies this.

- **NAT gateway idle timeout vs `--interval-ms`.** The gateway reaps a flow that
  is idle longer than its idle timeout; the next send then reconnects (churn).
  Keep `--interval-ms` comfortably below the idle timeout. The script sets the
  idle timeout to 30 minutes against a 1s default send interval, a wide margin.

## Cluster shape

- **Linux system pool** (2 x `Standard_D2s_v3`): AKS requires a Linux pool for
  system pods. Nothing in the workload runs here.
- **Windows user pool** (4 x `Standard_D8s_v3`, `os-sku Windows2022`): runs the
  load generator. 20 pods at ~250m CPU request spread ~5/node; Windows nodes
  carry heavy OS overhead, so the 8-vCPU size leaves room. The image base
  (`ltsc2022`) must match `os-sku Windows2022` because AKS uses process
  isolation, which requires a matching kernel.
- **Azure CNI overlay**: pod IPs come from a separate CIDR, so fanning out to
  dozens of pods does not consume VNet address space.
- Pods are spread across Windows nodes with a `topologySpreadConstraints` (in the
  Deployment) so no single node holds a disproportionate share of the 40K.

## Prerequisites

- `az` CLI, logged in (`az login`), and `kubectl`.
- The dedicated test Event Hubs connection string (with `EntityPath=<hub>`).
- For CI image push: an OIDC app registration with `AcrPush` on the ACR, wired
  into the repo as described in `.github/workflows/windows.yml`.

## Step 1: provision (greenfield)

```bash
# Optional overrides; defaults are in the script.
export LOCATION=eastus2 RG=eh-loadtest-rg AKS=eh-loadtest-aks
scripts/aks-40k-setup.sh
```

The script is idempotent (each resource is created only if absent). It prints the
generated ACR name at the end; save it. Windows node provisioning and the first
servercore image pull are slow (minutes); expect the first rollout to lag.

## Step 2: build and push the image

Windows containers cannot be built on Linux or by ACR Tasks, so the build runs on
the hosted `windows-2022` GitHub runner. The `docker` job builds the image, smoke
tests it, and (on `main`/tags, never on PRs) logs in via OIDC and pushes to ACR:

```
<acr>.azurecr.io/eh-loadtest:<git-sha>
<acr>.azurecr.io/eh-loadtest:latest
```

Set `vars.ACR_NAME`, `secrets.AZURE_CLIENT_ID`, `secrets.AZURE_TENANT_ID`, and
`secrets.AZURE_SUBSCRIPTION_ID` in the repo first. Then edit
`k8s/deployment.yaml` to replace `<acr>` and pin the git-sha tag.

## Step 3: secret + deploy

```bash
kubectl create secret generic eh-sas \
  --from-literal=connectionString='Endpoint=sb://<ns>.servicebus.windows.net/;SharedAccessKeyName=SendPolicy;SharedAccessKey=<key>;EntityPath=<hub>'

kubectl apply -f k8s/deployment.yaml
kubectl get pods -l app=eh-loadtest -o wide      # confirm spread across nodes
```

## Step 4: validate the run

Each pod prints the same metrics line the README describes; read it across the
fleet:

```bash
kubectl logs -l app=eh-loadtest --tail=2 -f
# [load] active=1980 connecting=4 rps=1975 ok=... failed=0 err{connect=0,timeout=0,http=0,io=0}
```

Healthy fleet: summed `active` approaches 40K, `rps` approaches ~40K (at
`--interval-ms 1000`), and `failed`/`err{}` stay near zero. A rising
`err{connect}` across pods is the SNAT/port symptom; a rising `err{http}` is the
endpoint pushing back (see Step 5).

The README's TIME_WAIT / socket-identity (`held`) checks still apply, run inside
a pod for the connection-reuse proof:

```bash
kubectl exec -it deploy/eh-loadtest -- powershell
# then run the held=/new=/gone= one-liner from the README
```

**Cloud-side SNAT view.** The per-host PowerShell counters only see one pod's
node; the authoritative fleet-wide signal is the NAT gateway's Azure Monitor
metrics: **SNAT Connection Count**, **Total SNAT Ports** / **SNAT port
utilization**, and **Dropped Packets**. Port utilization climbing toward 64,512
per IP, or any dropped packets, is SNAT exhaustion, add a third NAT gateway IP
(`az aks update --nat-gateway-managed-outbound-ip-count 3`).

## Step 5: don't let Event Hubs be the bottleneck

The point is to measure the gateway (or whatever fronts the endpoint), not to
discover Event Hubs limits. A dedicated **test** namespace has hard caps that 40K
connections will reach (verified against Event Hubs quotas):

- **Ingress throughput.** Standard tier is 1 MB/s or 1,000 events/s **per
  throughput unit**, max **40 TUs = 40,000 events/s**. At 40K connections x 1
  rps that is exactly the Standard ceiling, so any overshoot throttles
  (`ServerBusy` / HTTP 429), which surfaces as `err{http}` and reconnect churn,
  not a gateway signal. Mitigate by raising `--interval-ms` (e.g. 2000ms ->
  ~20K events/s -> ~20 TUs) or using Premium/Dedicated.
- **Brokered connections per namespace.** Standard 5,000; Premium 10,000 **per
  PU** (4 PU = 40,000); Dedicated 100,000 **per CU**. This counter is defined for
  persistent AMQP/Kafka/MQTT sessions; the tool uses the request-based **HTTP**
  send endpoint, so keep-alive TCP sockets are not the same thing. Still, confirm
  it on the namespace **Connections / Active Connections** metric before
  concluding 40K HTTP sockets are fine, the binding limit depends on whether you
  hit EH's HTTP endpoint directly or a gateway in front of it.

Watch the namespace metrics during the run: **Throttled Requests**, **Quota
Exceeded Errors**, **Incoming Requests/Messages**, **Active Connections**. Size
the tier (more TUs, or Premium PUs) so none of these is what you are measuring.

## Step 6: tear down

Windows nodes and a NAT gateway both bill while running. Delete the whole
resource group when the test is done:

```bash
RG=eh-loadtest-rg scripts/aks-40k-teardown.sh
```

## Quick troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| `err{connect}` rising fleet-wide, NAT port utilization near max | SNAT exhaustion | add a NAT gateway IP |
| `err{http}` rising, EH Throttled Requests climbing | EH TU/PU ceiling | raise `--interval-ms` or the tier |
| `failed` rising right after ramp | endpoint can't hold the conns, or churn from idle reaping | check NAT idle timeout vs `--interval-ms`; lengthen ramp |
| Pods stuck `ContainerCreating` for minutes | first servercore pull on a fresh node | wait; pre-pull on the pool if recurring |
| Pods `Pending` | image/node mismatch or no room | check `os-sku Windows2022` <-> `ltsc2022`, and node CPU |
