#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: gen-gateway-locator-config.sh [--profile DIR] [--node NAME] [--asn ASN]

Print, for each MIDR node of the profile, the FRR configuration its site
gateway (BGP-R) needs before MIDR routes are exported to it: advertise the
prefix holding the node's locator (/64 by default) into the underlay.

Why: once a MIDR edge node exports a remote site prefix such as
2001:db8:b::/48 to its gateway, that prefix also covers the remote locator
2001:db8:b:1::2.  Without a more specific underlay route the gateway sends
GRE and control traffic for the locator back to the MIDR node, which hands
it to the gateway again.  The longer underlay prefix keeps locator traffic
on the underlay; business traffic still matches the exported /48.

The prefix must be a connected (or otherwise installed) route on the
gateway, and the underlay must carry it between sites.  This script only
prints configuration; apply it on each gateway after review, for example
with "vtysh -f FILE" followed by "write memory".

--node NAME  only this node (default: every node)
--asn ASN    gateway AS number to put in "router bgp" (default: a placeholder)
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=lib/profile.sh
. "$SCRIPT_DIR/lib/profile.sh"
PROFILE_DIR=""
NODE=""
ASN="<gateway-AS>"

need_value() {
    [ "$2" -ge 2 ] || { echo "$1 requires a value" >&2; exit 2; }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --profile) need_value "$1" "$#"; PROFILE_DIR=$2; shift 2 ;;
        --node) need_value "$1" "$#"; NODE=$2; shift 2 ;;
        --asn) need_value "$1" "$#"; ASN=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
midr_profile_load "$PROFILE_DIR"

found=0
while IFS=, read -r name locator gateway; do
    [ -z "$NODE" ] || [ "$name" = "$NODE" ] || continue
    found=1
    prefix=$(midr_locator_prefix "$locator")
    case "$prefix" in
        *:*) family=ipv6 ;;
        *) family=ipv4 ;;
    esac
    cat <<EOF
! $name: gateway $gateway advertises the locator prefix of $locator
router bgp $ASN
 address-family $family unicast
  network $prefix
 exit-address-family
exit
!
! Recommended inbound policy for the routes $name exports (adapt names and
! preference to the site): keep them inside the gateway AS, and prefer them
! over the underlay so business traffic enters MIDR.
!   route-map FROM-MIDR permit 10
!    set community no-export additive
!    set local-preference 200
!   router bgp $ASN
!    address-family $family unicast
!     neighbor $locator route-map FROM-MIDR in
EOF
done < <(midr_nodes_select name locator gateway)
[ "$found" -eq 1 ] || { echo "node not in $MIDR_NODES_FILE: $NODE" >&2; exit 1; }
