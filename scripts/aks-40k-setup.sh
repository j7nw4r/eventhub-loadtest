#!/usr/bin/env bash
#
# Greenfield AKS provisioning for a ~40,000-connection eventhub-loadtest run.
#
# Stands up: a resource group, an ACR, and an AKS cluster with a small Linux
# system pool plus a Windows user pool that runs the load generator. Egress goes
# through a managed NAT gateway, NOT the default load balancer, because all 40K
# sockets target a single destination (the Event Hubs host on :443) and the LB's
# per-node SNAT port budget is far too small for that. A NAT gateway provides
# 64,512 SNAT ports per public IP; two IPs give ~129K, comfortable headroom over
# 40K. The Windows pool is created with OutboundNAT disabled so Windows does not
# do its own pod->node port translation on top of the NAT gateway (that double
# translation is itself a port-exhaustion source). See docs/aks-40k.md.
#
# Idempotent: each step checks for the resource first, so re-running is safe.
# Requires: az CLI (logged in: `az login`), kubectl.
#
# Authorized use only. This provisions infrastructure that generates heavy,
# sustained traffic. Point it only at endpoints you own or are authorized to test.

set -euo pipefail

# --- Knobs (override via environment) ----------------------------------------
LOCATION="${LOCATION:-eastus2}"
RG="${RG:-eh-loadtest-rg}"
ACR="${ACR:-ehloadtest$RANDOM}"          # must be globally unique, 5-50 alnum
AKS="${AKS:-eh-loadtest-aks}"

# Linux system pool (required by AKS for system pods; nothing in our workload
# runs here). Kept small.
SYS_NODE_SIZE="${SYS_NODE_SIZE:-Standard_D2s_v3}"
SYS_NODE_COUNT="${SYS_NODE_COUNT:-2}"

# Windows user pool that runs eh-loadtest. 20 pods x 2000 connections = 40K, so
# size for ~5 pods/node across 4 nodes. Windows nodes carry heavy OS overhead;
# D8s_v3 (8 vCPU / 32 GB) leaves room after that.
WIN_POOL="${WIN_POOL:-win}"
WIN_NODE_SIZE="${WIN_NODE_SIZE:-Standard_D8s_v3}"
WIN_NODE_COUNT="${WIN_NODE_COUNT:-4}"
WIN_OS_SKU="${WIN_OS_SKU:-Windows2022}"  # must match the image base (ltsc2022)

# NAT gateway egress. idle-timeout (minutes) must exceed --interval-ms of the
# load test with margin, or the gateway reaps idle keep-alive flows and the next
# send reconnects (churn the tool will report). 30 min is safe for sub-minute
# send intervals.
NAT_IP_COUNT="${NAT_IP_COUNT:-2}"
NAT_IDLE_TIMEOUT="${NAT_IDLE_TIMEOUT:-30}"

echo "== config =="
echo "  location=$LOCATION rg=$RG acr=$ACR aks=$AKS"
echo "  system: $SYS_NODE_COUNT x $SYS_NODE_SIZE"
echo "  windows: $WIN_NODE_COUNT x $WIN_NODE_SIZE ($WIN_OS_SKU) pool=$WIN_POOL"
echo "  nat: $NAT_IP_COUNT ip(s), idle=${NAT_IDLE_TIMEOUT}m"
echo

# --- Resource group ----------------------------------------------------------
if ! az group show --name "$RG" >/dev/null 2>&1; then
  echo "== creating resource group $RG =="
  az group create --name "$RG" --location "$LOCATION" --output none
fi

# --- Container registry ------------------------------------------------------
if ! az acr show --name "$ACR" >/dev/null 2>&1; then
  echo "== creating ACR $ACR =="
  az acr create --resource-group "$RG" --name "$ACR" --sku Basic --output none
fi

# --- AKS cluster: Linux system pool + managed NAT gateway egress -------------
# Azure CNI overlay scales pod IPs without consuming VNet address space, which
# matters once you fan out to dozens of pods. managedNATGateway sets the cluster
# outbound type to NAT gateway (a prerequisite for --disable-windows-outbound-nat
# on the Windows pool below).
if ! az aks show --resource-group "$RG" --name "$AKS" >/dev/null 2>&1; then
  echo "== creating AKS $AKS (this takes several minutes) =="
  az aks create \
    --resource-group "$RG" \
    --name "$AKS" \
    --location "$LOCATION" \
    --node-count "$SYS_NODE_COUNT" \
    --node-vm-size "$SYS_NODE_SIZE" \
    --network-plugin azure \
    --network-plugin-mode overlay \
    --outbound-type managedNATGateway \
    --nat-gateway-managed-outbound-ip-count "$NAT_IP_COUNT" \
    --nat-gateway-idle-timeout "$NAT_IDLE_TIMEOUT" \
    --attach-acr "$ACR" \
    --generate-ssh-keys \
    --output none
fi

# --- Windows user pool with OutboundNAT disabled -----------------------------
# Disabling Windows OutboundNAT requires the cluster outbound type to be NAT
# gateway (set above) and is what keeps Windows from layering its own SNAT on top
# of the gateway.
if ! az aks nodepool show --resource-group "$RG" --cluster-name "$AKS" \
      --name "$WIN_POOL" >/dev/null 2>&1; then
  echo "== adding Windows node pool $WIN_POOL =="
  az aks nodepool add \
    --resource-group "$RG" \
    --cluster-name "$AKS" \
    --name "$WIN_POOL" \
    --os-type Windows \
    --os-sku "$WIN_OS_SKU" \
    --node-vm-size "$WIN_NODE_SIZE" \
    --node-count "$WIN_NODE_COUNT" \
    --disable-windows-outbound-nat \
    --output none
fi

# --- kubeconfig --------------------------------------------------------------
echo "== fetching credentials =="
az aks get-credentials --resource-group "$RG" --name "$AKS" --overwrite-existing

cat <<EOF

Done. Cluster is up. Next:

  1. Push the image (CI does this on main; or build on a Windows host):
       $ACR.azurecr.io/eh-loadtest:<tag>
     Set that image in k8s/deployment.yaml (or via kustomize/sed).

  2. Provide the dedicated test Event Hubs connection string (never committed;
     supplied via the EH_CONNECTION_STRING env var, same as the tool locally):
       export EH_CONNECTION_STRING='Endpoint=sb://<ns>.servicebus.windows.net/;SharedAccessKeyName=SendPolicy;SharedAccessKey=<key>;EntityPath=<hub>'
       scripts/aks-set-secret.sh
     Or resolve it from 1Password without it touching your shell history:
       export EH_CONNECTION_STRING='op://Private/EventHub Test/connection string'
       op run -- scripts/aks-set-secret.sh

  3. Deploy and watch it ramp:
       kubectl apply -f k8s/deployment.yaml
       kubectl get pods -l app=eh-loadtest -o wide
       kubectl logs -l app=eh-loadtest --tail=2 -f

  Registry name (save it): $ACR

Tear everything down with: scripts/aks-40k-teardown.sh  (RG=$RG)
EOF
