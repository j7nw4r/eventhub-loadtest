#!/usr/bin/env bash
#
# Create or update the Kubernetes Secret that holds the dedicated test Event Hubs
# connection string. The Deployment reads it as EH_CONNECTION_STRING (see
# k8s/deployment.yaml: secretKeyRef name=eh-sas key=connectionString).
#
# The value comes from the EH_CONNECTION_STRING environment variable, the same
# knob the tool uses locally, so it never lands in a manifest or your shell
# history. Two ways to supply it:
#
#   # A) plain env var (e.g. from a secure paste):
#   export EH_CONNECTION_STRING='Endpoint=sb://<ns>.servicebus.windows.net/;SharedAccessKeyName=SendPolicy;SharedAccessKey=<key>;EntityPath=<hub>'
#   scripts/aks-set-secret.sh
#
#   # B) 1Password reference resolved only inside the wrapped process:
#   export EH_CONNECTION_STRING='op://Private/EventHub Test/connection string'
#   op run -- scripts/aks-set-secret.sh
#
# Idempotent: re-running replaces the value (rotate the string without touching
# the Deployment; restart pods afterwards to pick it up, see below). Requires a
# kubectl context already pointed at the cluster (aks-40k-setup.sh runs
# get-credentials for you).

set -euo pipefail

SECRET_NAME="${SECRET_NAME:-eh-sas}"
SECRET_KEY="${SECRET_KEY:-connectionString}"

if [[ -z "${EH_CONNECTION_STRING:-}" ]]; then
  echo "EH_CONNECTION_STRING is not set." >&2
  echo "Set it (or wrap this script in 'op run --' for an op:// reference)." >&2
  exit 1
fi

# Sanity-check it looks like an EH connection string, and warn if the hub is
# missing (without EntityPath the tool can sign but not pick a hub).
case "$EH_CONNECTION_STRING" in
  Endpoint=sb://*) : ;;
  op://*)
    echo "EH_CONNECTION_STRING is still an op:// reference; run under 'op run --'." >&2
    exit 1 ;;
  *)
    echo "warning: EH_CONNECTION_STRING does not start with 'Endpoint=sb://'." >&2 ;;
esac
case "$EH_CONNECTION_STRING" in
  *EntityPath=*) : ;;
  *) echo "warning: no EntityPath=<hub> in the connection string; set --target in the Deployment, or add EntityPath." >&2 ;;
esac

# Write the value to a private temp file (avoids exposing the secret in the
# process list, which --from-literal would do) and apply via dry-run | apply so
# the same command creates or updates. printf with no newline so the stored
# value has no trailing \n.
umask 077
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
printf '%s' "$EH_CONNECTION_STRING" > "$tmp"

kubectl create secret generic "$SECRET_NAME" \
  --from-file="$SECRET_KEY=$tmp" \
  --dry-run=client -o yaml | kubectl apply -f -

echo "Secret '$SECRET_NAME' (key '$SECRET_KEY') is set."
echo "If pods are already running, restart them to pick up a changed value:"
echo "  kubectl rollout restart deploy/eh-loadtest"
