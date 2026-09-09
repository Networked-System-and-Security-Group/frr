#!/usr/bin/env bash

set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "Run this script with sudo." >&2
    exit 1
fi

NAMESPACES=(
    midr-af-nds4
    midr-af-nds6
    midr-pm4-a
    midr-pm4-b
    midr-pm6-a
    midr-pm6-b
    midr-pms4-a
    midr-pms4-b
    midr-pms6-a
    midr-pms6-b
)

namespace_exists() {
    ip netns list | grep -Eq "^$1([[:space:]]|$)"
}

for namespace in "${NAMESPACES[@]}"; do
    if ! namespace_exists "$namespace"; then
        continue
    fi
    mapfile -t pids < <(ip netns pids "$namespace")
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 0.2
    mapfile -t pids < <(ip netns pids "$namespace")
    for pid in "${pids[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    ip netns delete "$namespace"
    echo "Removed namespace: $namespace"
done

echo "MIDR address-family test resources are clean."
