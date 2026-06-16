#!/usr/bin/env bash
#
# Tear down everything created by aks-40k-setup.sh. Deleting the resource group
# removes the AKS cluster, the node pools, the managed NAT gateway, its public
# IPs, and the ACR in one shot. Windows nodes and a NAT gateway both bill while
# running, so run this as soon as the test is over.

set -euo pipefail

RG="${RG:-eh-loadtest-rg}"

echo "This deletes the entire resource group '$RG' and everything in it"
echo "(AKS cluster, Windows node pool, NAT gateway, public IPs, ACR)."
read -r -p "Type the resource group name to confirm: " confirm
if [[ "$confirm" != "$RG" ]]; then
  echo "Aborted."
  exit 1
fi

az group delete --name "$RG" --yes --no-wait
echo "Deletion started (running in the background)."
echo "Check with: az group show --name $RG   (404 once gone)"
