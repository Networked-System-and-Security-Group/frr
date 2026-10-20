#!/usr/bin/env bash
# Reverse export with the generated MIDR configuration, then the locator
# loop it prevents: withdraw ma's locator /64, watch the loop, restore it.
#
#   RELEASE=/tmp/midr-<id> midr-test/route-export-lab/test.sh
#
# Needs containerlab, the frr-midr-p6 image and a release built by
# tools/midr-deploy/build-release.sh.  Work files go to MIDR_LAB_WORK
# (default /tmp/midr-route-export-lab); the lab is left running for
# inspection, remove it with
#   containerlab destroy -t /tmp/midr-route-export-lab/midr-loop.clab.yaml --cleanup
set -uo pipefail
SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
LAB=${MIDR_LAB_WORK:-/tmp/midr-route-export-lab}
: "${RELEASE:?set RELEASE to a midr-* release directory}"
TOOLS=${TOOLS:-$(CDPATH= cd -- "$SRC/../../tools/midr-deploy" && pwd -P)}
export RELEASE TOOLS MIDR_LAB_WORK=$LAB
mkdir -p "$LAB"
PASS=0; FAIL=0
ok() { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
X() { docker exec "clab-midr-loop-$1" bash -c "$2"; }
VT() { X "$1" "LD_LIBRARY_PATH=/opt/midr/lib:/opt/hostlib /opt/midr/vtysh --vty_socket /run/midr $2 2>/dev/null"; }
# gwb egress for a destination: eth1 leads to mb (MIDR), eth2 to the underlay.
nh() { X gwb "ip -6 route get $1" 2>&1 | grep -o 'dev [^ ]*' | head -1; }
ping_ok() { X gwb "ping -6 -c1 -W1 -I 2001:db8:b:10::1 2001:db8:a:10::1" >/dev/null 2>&1; }
session_up() { X ma "python3 /opt/midr/midrd-vty.py --socket /run/midr/midrd.vty -c 'show midr neighbors'" 2>/dev/null | grep -q Established; }
gre_tx() { X mb 's=0; for d in /sys/class/net/*; do [ "$(cat $d/type)" = 823 ] && s=$((s + $(cat $d/statistics/tx_packets))); done; echo $s'; }

sample() {  # label, rounds: locator egress, session and business ping
    local label=$1 rounds=$2 loop=0 down=0 lost=0 i
    for ((i = 0; i < rounds; i++)); do
        [ "$(nh 2001:db8:a:1::2)" = "dev eth1" ] && loop=$((loop + 1))
        session_up || down=$((down + 1))
        ping_ok || lost=$((lost + 1))
        sleep 2
    done
    echo "$label: rounds=$rounds locator-via-MIDR=$loop session-down=$down ping-lost=$lost"
    SAMPLE="$loop $down $lost"
}

converge() {  # wait for the overlay path and the locator underlay route
    local i
    for i in $(seq 90); do
        [ "$(nh 2001:db8:a:1::2)" = "dev eth2" ] && [ "$(nh 2001:db8:a:10::1)" = "dev eth1" ] && \
            session_up && ping_ok && return 0
        sleep 2
    done
    return 1
}

steady_checks() {  # label
    local before after
    X gwb "ip -6 route get 2001:db8:a:1::2; ip -6 route get 2001:db8:a:10::1"
    sample "$1" 30
    read -r loop down lost <<<"$SAMPLE"
    [ "$loop" -eq 0 ] && ok "$1: gwb reaches the remote locator through the underlay" || bad "$1: locator routed via MIDR in $loop samples"
    [ "$down" -eq 0 ] && ok "$1: MIDR session stayed Established for 60 s" || bad "$1: session down in $down samples"
    [ "$lost" -eq 0 ] && ok "$1: business ping b:10::1 -> a:10::1 never lost" || bad "$1: ping lost $lost/30"
    [ "$(nh 2001:db8:a:10::1)" = "dev eth1" ] && ok "$1: business traffic enters MIDR at mb" || bad "$1: business traffic bypasses MIDR"
    before=$(gre_tx)
    X gwb "ping -6 -c20 -i0.05 -W1 -q -I 2001:db8:b:10::1 2001:db8:a:10::1" >/dev/null
    after=$(gre_tx)
    [ "$((after - before))" -ge 20 ] && ok "$1: pings crossed the GRE virtual link (tx +$((after - before)))" || bad "$1: GRE counters did not grow ($before -> $after)"
}

containerlab destroy -t "$LAB/midr-loop.clab.yaml" --cleanup >/dev/null 2>&1
bash "$SRC/gen.sh" >/dev/null
( . "$TOOLS/lib/profile.sh"; midr_profile_load "$LAB/profile" >/dev/null; midr_locator_prefixes ) >"$LAB/locators.txt"
containerlab deploy -t "$LAB/midr-loop.clab.yaml" >/dev/null 2>&1 || { echo 'deploy failed'; exit 1; }
for n in gwa gwb ma mb; do X $n "bash /lab/node.sh $n"; done

echo '== Generated export configuration on ma'
X ma "sed -n '/MIDR route export/,\$p' /etc/midr/bgpd.conf"

echo '== 1. Generated configuration'
converge || bad "did not converge in 180 s"
X mb "ip -6 route show proto 199" | grep -q '2001:db8:a::/48' && ok "mb installed site A /48 from MIDR (proto 199)" || bad "mb has no MIDR route for site A"
VT gwb "-c 'show bgp ipv6 unicast 2001:db8:a::/48'" >"$LAB/gwb-a48.txt"
grep -q 'from 2001:db8:b:1::2' "$LAB/gwb-a48.txt" && grep -q 'best' "$LAB/gwb-a48.txt" && ok "gwb learned site A /48 from mb over iBGP" || bad "gwb did not learn the exported route"
grep -q 'no-export' "$LAB/gwb-a48.txt" && ok "exported route carries no-export" || bad "no-export missing"
VT gwa "-c 'show bgp ipv6 unicast 2001:db8:a::/48'" | grep -q '65002' && bad "gwb leaked the MIDR route to gwa" || ok "gwb kept the MIDR route inside its AS"
VT gwb "-c 'show bgp ipv6 unicast 2001:db8:a:1::/64'" | grep -q '65001' && ok "gwb learned ma's locator /64 from gwa (announced by ma)" || bad "locator /64 not propagated"
VT ma "-c 'show bgp ipv6 unicast 2001:db8:b:1::/64'" | grep -q 'valid' && ok "ma received the remote locator /64 from gwa" || bad "ma did not receive the remote locator /64"
X mb "ip -6 route show proto 199" | grep -qE '2001:db8:[ab]:1::/64' && bad "a locator /64 leaked into MIDR" || ok "no locator /64 imported into MIDR (Provider exclusion)"
steady_checks steady

echo '== 2. Withdraw ma locator /64 (old behaviour)'
VT ma "-c 'configure terminal' -c 'router bgp 65001' -c 'address-family ipv6 unicast' -c 'no network 2001:db8:a:1::/64'" >/dev/null
sleep 5
X gwb "ip -6 route get 2001:db8:a:1::2"
sample withdrawn 20
read -r loop down lost <<<"$SAMPLE"
[ "$loop" -gt 0 ] && ok "loop reproduced: gwb sends remote-locator traffic back to mb ($loop/20)" || bad "loop not reproduced"
{ [ "$down" -gt 0 ] || [ "$lost" -gt 0 ]; } && ok "control session or business ping disrupted (down=$down lost=$lost)" || bad "no disruption seen"

echo '== 3. Restore the generated configuration'
VT ma "-c 'configure terminal' -c 'router bgp 65001' -c 'address-family ipv6 unicast' -c 'network 2001:db8:a:1::/64'" >/dev/null
converge || bad "did not recover in 180 s"
sleep 10
steady_checks restored
echo '== mb MIDR routes'; X mb "ip -6 route show proto 199"
echo "RESULT pass=$PASS fail=$FAIL"
