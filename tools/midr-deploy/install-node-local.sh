#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo 'Usage: install-node-local.sh STAGE check|apply' >&2
    exit 2
fi
STAGE=$1
MODE=$2
case "$MODE" in check|apply) ;; *) echo 'mode must be check or apply' >&2; exit 2 ;; esac
source "$STAGE/node.env"
RELEASE="$STAGE/release"
DEST="$MIDR_RELEASE_DIR"
: "${MIDR_NODE_NAME:?}" "${MIDR_LOCATOR:?}" "${MIDR_GATEWAY:?}"
if [ "$MIDR_ROLE" = bootstrap ] && \
   { [ "$MIDR_EDGE_BGP" != no ] || [ "$MIDR_PREFIX_MODE" != none ]; }; then
    echo 'dedicated bootstrap requires edge-bgp=no and prefix-mode=none' >&2
    exit 1
fi
if [ "$MIDR_EDGE_BGP" = yes ] && [ "$MIDR_PREFIX_MODE" != socket ]; then
    echo 'edge MIDR requires prefix-mode=socket' >&2
    exit 1
fi

[ "$(id -u)" -eq 0 ] || { echo 'deployment requires root' >&2; exit 1; }
# Interface carrying the transport-address; setup-midr-underlay.py reads the
# same variable from node.env.
UNDERLAY_IF=${MIDR_UNDERLAY_INTERFACE:-ens5}
python3 - "$MIDR_LOCATOR" "$UNDERLAY_IF" <<'PY'
import json, subprocess, sys
locator, device = sys.argv[1], sys.argv[2]
addresses = json.loads(subprocess.check_output(["ip", "-j", "-6", "addr", "show", "dev", device]))
locals_ = {a["local"] for interface in addresses for a in interface.get("addr_info", [])}
if locator not in locals_:
    raise SystemExit(f"{device} does not carry this node's transport-address {locator}")
PY
# Platform-owned netplan files (MIDR_PROTECTED_NETPLAN, empty by default) must
# exist now and be unchanged after deployment.
read -r -a PROTECTED_NETPLAN <<<"${MIDR_PROTECTED_NETPLAN:-}"
for file in "${PROTECTED_NETPLAN[@]}"; do
    [ -r "/etc/netplan/$file" ] || { echo "missing protected netplan file: /etc/netplan/$file" >&2; exit 1; }
done
# A system FRR is optional.  Edge nodes need a BGP configuration: the
# profile's node.bgpd.conf, else the system FRR's bgpd.conf or frr.conf.
BGP_CONFIG_SOURCE=""
for candidate in "$STAGE/node.bgpd.conf" /etc/frr/bgpd.conf /etc/frr/frr.conf; do
    if [ -r "$candidate" ]; then
        BGP_CONFIG_SOURCE=$candidate
        break
    fi
done
if [ "$MIDR_EDGE_BGP" = yes ] && \
   { [ -z "$BGP_CONFIG_SOURCE" ] || \
     ! grep -Eq '^[[:space:]]*router bgp [0-9]+' "$BGP_CONFIG_SOURCE"; }; then
    echo "edge MIDR is missing router bgp configuration: ${BGP_CONFIG_SOURCE:-none found}" >&2
    exit 1
fi
ip -6 route show default | grep -F "via $MIDR_GATEWAY dev $UNDERLAY_IF"
(cd "$RELEASE" && sha256sum -c SHA256SUMS)
# Check reused release directories before stopping any running service.
if [ -f "$DEST/midrd" ]; then
    [ -d "$DEST/lib" ] || {
        echo "existing release is missing lib/: $DEST; repair or select a new release directory before applying" >&2
        exit 1
    }
    if ! (cd "$DEST" && sha256sum -c "$RELEASE/SHA256SUMS"); then
        echo "existing release is incomplete or differs from the uploaded package: $DEST; repair or select a new release directory before applying" >&2
        exit 1
    fi
    for binary in midrd zebra bgpd vtysh; do
        if ! file -b "$DEST/$binary" 2>/dev/null | grep -Fq 'ELF'; then
            continue
        fi
        existing_dependencies=$(LD_LIBRARY_PATH="$DEST/lib" ldd "$DEST/$binary" 2>&1)
        if printf '%s\n' "$existing_dependencies" | grep -Fq 'not found'; then
            printf '%s\n' "$existing_dependencies" >&2
            echo "existing release has missing shared-library dependencies: $DEST/$binary" >&2
            exit 1
        fi
    done
fi
for binary in midrd zebra bgpd vtysh; do
    if ! file -b "$RELEASE/$binary" 2>/dev/null | grep -Fq 'ELF'; then
        continue
    fi
    dependency_output=$(LD_LIBRARY_PATH="$RELEASE/lib" ldd "$RELEASE/$binary" 2>&1)
    if printf '%s\n' "$dependency_output" | grep -Fq 'not found'; then
        printf '%s\n' "$dependency_output" >&2
        exit 1
    fi
done
# The current FITI gateway and interface routes belong to Netplan.
# Refuse to discard any additional protocol or static route configuration.
if [ "${MIDR_OSPF6_LEAF_MIGRATION:-no}" != yes ] && [ -r /etc/frr/frr.conf ] && \
   grep -Eq '^[[:space:]]*(ipv6 route|ip route|router ospf|router isis|router rip)' /etc/frr/frr.conf; then
    echo 'FRR includes additional routing configuration; migrate it before applying this deployment' >&2
    exit 1
fi
# Reverse export of MIDR routes to the site gateway (bgpd only for now).
case "${MIDR_ROUTE_EXPORT:-no}" in
    no) ;;
    yes)
        [ "$MIDR_EDGE_BGP" = yes ] || { echo 'MIDR_ROUTE_EXPORT=yes requires an edge node' >&2; exit 1; }
        [ -s "$STAGE/node.locators" ] || { echo 'route export needs the profile locator list' >&2; exit 1; }
        bash "$STAGE/midr-export-config.sh" "$BGP_CONFIG_SOURCE" "$STAGE/node.locators" \
            "$MIDR_LOCATOR" "${MIDR_ROUTE_TAG:-199}" >/dev/null
        ;;
    *) echo 'MIDR_ROUTE_EXPORT must be yes or no' >&2; exit 1 ;;
esac
if [ "$MODE" = check ]; then
    printf 'CHECK PASS %s node-id=%s role=%s edge-bgp=%s\n' \
        "$MIDR_NODE_NAME" "$MIDR_NODE_ID" "$MIDR_ROLE" "$MIDR_EDGE_BGP"
    exit 0
fi

BACKUP="/var/lib/midr/deploy-backups/$(date -u +%Y%m%dT%H%M%SZ)-$MIDR_NODE_NAME-$$"
install -d -m 700 "$BACKUP"
if [ -d /etc/frr ]; then cp -a /etc/frr "$BACKUP/frr"; fi
install -d -m 700 "$BACKUP/netplan"
for file in "${PROTECTED_NETPLAN[@]}"; do
    cp -a "/etc/netplan/$file" "$BACKUP/netplan/$file"
done
if [ -d /etc/midr ]; then cp -a /etc/midr "$BACKUP/midr"; fi
for file in /etc/systemd/system/midr-logrotate.service \
    /etc/systemd/system/midr-logrotate.timer /etc/needrestart/conf.d/midr.conf; do
    if [ -f "$file" ]; then cp -a "$file" "$BACKUP/"; fi
done
systemctl is-enabled frr >"$BACKUP/frr-enabled.txt" 2>&1 || true
systemctl is-active frr >"$BACKUP/frr-active.txt" 2>&1 || true
for unit in midr-zebra.service midr-bgpd.service midrd.service midr-bgp-prefix-provider.service; do
    systemctl is-enabled "$unit" >"$BACKUP/$unit-enabled.txt" 2>&1 || true
    systemctl is-active "$unit" >"$BACKUP/$unit-active.txt" 2>&1 || true
    if [ -f "/etc/systemd/system/$unit" ]; then
        cp -a "/etc/systemd/system/$unit" "$BACKUP/"
    fi
done
if [ -L /opt/midr-current ]; then
    readlink -f /opt/midr-current >"$BACKUP/midr-current.txt"
fi
pgrep -a midrd >"$BACKUP/legacy-midrd.txt" || true
pgrep -a zebra >"$BACKUP/legacy-zebra.txt" || true
pgrep -a bgpd >"$BACKUP/legacy-bgpd.txt" || true
ip -6 rule show >"$BACKUP/ipv6-rules.txt"
ip -6 route show table all >"$BACKUP/ipv6-routes.txt"
ip -4 route show table all >"$BACKUP/ipv4-routes.txt"

stop_daemon() {
    local daemon=$1 pid
    local -a pids=()
    mapfile -t pids < <(pgrep -x "$daemon" || true)
    for pid in "${pids[@]}"; do
        local exe
        exe=$(readlink -f "/proc/$pid/exe") || continue
        case "$exe" in
            /opt/midr-releases/*/"$daemon"|/opt/midr-*/"$daemon"|/usr/lib/frr/"$daemon") ;;
            *) echo "unexpected $daemon executable: $exe" >&2; return 1 ;;
        esac
        kill -TERM "$pid"
    done
    for ((attempt = 0; attempt < 40; attempt++)); do
        if ! pgrep -x "$daemon" >/dev/null; then return 0; fi
        sleep 1
    done
    echo "$daemon did not terminate" >&2
    return 1
}

rollback_on_error() {
    local rc=$?
    trap - ERR
    set +e
    printf 'DEPLOYMENT FAILED; restoring system FRR; backup=%s\n' "$BACKUP" >&2
    systemctl stop midr-bgp-prefix-provider.service midrd.service midr-bgpd.service midr-zebra.service
    systemctl disable midr-bgp-prefix-provider.service midrd.service midr-bgpd.service midr-zebra.service
    for unit in midr-zebra.service midr-bgpd.service midrd.service midr-bgp-prefix-provider.service; do
        if [ -f "$BACKUP/$unit" ]; then
            cp -a "$BACKUP/$unit" "/etc/systemd/system/$unit"
        else
            rm -f "/etc/systemd/system/$unit"
        fi
    done
    for file in midr-logrotate.service midr-logrotate.timer; do
        if [ -f "$BACKUP/$file" ]; then
            cp -a "$BACKUP/$file" "/etc/systemd/system/$file"
        else
            systemctl disable --now midr-logrotate.timer 2>/dev/null
            rm -f "/etc/systemd/system/$file"
        fi
    done
    if [ -f "$BACKUP/midr.conf" ]; then
        cp -a "$BACKUP/midr.conf" /etc/needrestart/conf.d/midr.conf
    else
        rm -f /etc/needrestart/conf.d/midr.conf
    fi
    systemctl daemon-reload
    if [ -d "$BACKUP/frr" ]; then
        cp -a "$BACKUP/frr/." /etc/frr/
    fi
    if [ -d "$BACKUP/midr" ]; then
        cp -a "$BACKUP/midr/." /etc/midr/
    fi
    if grep -qx enabled "$BACKUP/frr-enabled.txt"; then systemctl enable frr; fi
    if grep -qx active "$BACKUP/frr-active.txt"; then systemctl start frr; fi
    if [ -f "$BACKUP/midr-zebra.service" ] && grep -qx active "$BACKUP/midr-zebra.service-active.txt"; then
        systemctl start midr-zebra.service
    fi
    if [ -f "$BACKUP/midr-bgpd.service" ] && grep -qx active "$BACKUP/midr-bgpd.service-active.txt"; then
        systemctl start midr-bgpd.service
    fi
    if [ -f "$BACKUP/midrd.service" ] && grep -qx active "$BACKUP/midrd.service-active.txt"; then
        systemctl start midrd.service
    fi
    if [ -f "$BACKUP/midr-bgp-prefix-provider.service" ] && grep -qx active "$BACKUP/midr-bgp-prefix-provider.service-active.txt"; then
        systemctl start midr-bgp-prefix-provider.service
    fi
    if [ -s "$BACKUP/midr-current.txt" ]; then
        ln -sfn "$(cat "$BACKUP/midr-current.txt")" /opt/midr-current
    fi
    echo 'legacy two-node experiment arguments are saved in legacy-midrd.txt' >&2
    exit "$rc"
}
trap rollback_on_error ERR

# Stop the old MIDR before stopping Zebra so MIDR can withdraw its FIB.
systemctl stop midr-bgp-prefix-provider.service midrd.service midr-bgpd.service midr-zebra.service || true
systemctl stop frr || true
systemctl disable frr || true
for ((attempt = 0; attempt < 40; attempt++)); do
    if ! systemctl is-active --quiet frr; then break; fi
    sleep 1
done
stop_daemon midrd
stop_daemon bgpd
stop_daemon zebra
if pgrep -x zebra >/dev/null || pgrep -x bgpd >/dev/null || pgrep -x midrd >/dev/null; then
    echo 'old routing daemon is still running after stop; refusing to start the new services' >&2
    exit 1
fi

install -d -m 755 "$DEST" /etc/midr /run/midr /var/log/midr /usr/local/libexec/midr
# Existing release files must match the uploaded manifest. A new release is
# copied as a complete unit; the old release directory is retained for
# rollback and is never overwritten.
if [ -f "$DEST/midrd" ]; then
    (cd "$DEST" && sha256sum -c "$RELEASE/SHA256SUMS")
else
    cp -a "$RELEASE/." "$DEST/"
fi
(cd "$DEST" && sha256sum -c "$RELEASE/SHA256SUMS")
cp -a "$RELEASE/SHA256SUMS" "$DEST/SHA256SUMS"
install -m 600 "$STAGE/node.env" /etc/midr/node.env
install -m 600 "$STAGE/node.commands" /etc/midr/node.commands
if [ -n "$BGP_CONFIG_SOURCE" ]; then
    install -m 600 "$BGP_CONFIG_SOURCE" /etc/midr/bgpd.conf
fi
# Locator prefixes of every MIDR node: never imported as MIDR prefixes by
# the Prefix Provider, never exported to the gateway.
if [ -s "$STAGE/node.locators" ]; then
    install -m 644 "$STAGE/node.locators" /etc/midr/locators.txt
fi
if [ "${MIDR_ROUTE_EXPORT:-no}" = yes ]; then
    bash "$STAGE/midr-export-config.sh" /etc/midr/bgpd.conf /etc/midr/locators.txt \
        "$MIDR_LOCATOR" "${MIDR_ROUTE_TAG:-199}" >>/etc/midr/bgpd.conf
fi
printf 'hostname %s-zebra\n' "$MIDR_NODE_NAME" >/etc/midr/zebra.conf
chmod 600 /etc/midr/zebra.conf
for script in start-midr-zebra.sh wait-midr-zebra.sh start-midr-bgpd.sh \
    start-midrd-service.sh load-midrd-config.sh \
    start-midr-bgp-prefix-provider.sh; do
    install -m 755 "$STAGE/$script" "/usr/local/libexec/midr/$script"
done
install -m 755 "$STAGE/setup-midr-underlay.py" \
    /usr/local/libexec/midr/setup-midr-underlay.py
install -m 755 "$RELEASE/midr-bgp-prefix-provider.py" \
    /usr/local/libexec/midr/midr-bgp-prefix-provider.py
for unit in midr-zebra.service midr-bgpd.service midrd.service midr-bgp-prefix-provider.service \
    midr-logrotate.service midr-logrotate.timer; do
    install -m 644 "$STAGE/$unit" "/etc/systemd/system/$unit"
done
# Size-based rotation of /var/log/midr (the daemons append through shell
# redirection, so copytruncate needs no service restart).
install -m 644 "$STAGE/midr-logrotate.conf" /etc/midr/logrotate.conf
# Keep unattended upgrades from restarting the MIDR services at random times.
if [ -d /etc/needrestart ]; then
    install -d -m 755 /etc/needrestart/conf.d
    install -m 644 "$STAGE/needrestart-midr.conf" /etc/needrestart/conf.d/midr.conf
fi
# All previous daemons are gone; only stale runtime sockets are removed.
rm -f /run/midr/zserv.api /run/midr/zebra.vty /run/midr/bgpd.vty \
    /run/midr/midrd.vty /run/midr/prefix.sock /run/midr/zebra.pid \
    /run/midr/bgpd.pid "/run/midr/midrd-$MIDR_NODE_ID.pid"
systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/midr-zebra.service \
    /etc/systemd/system/midr-bgpd.service /etc/systemd/system/midrd.service \
    /etc/systemd/system/midr-bgp-prefix-provider.service \
    /etc/systemd/system/midr-logrotate.service /etc/systemd/system/midr-logrotate.timer
export LD_LIBRARY_PATH="$DEST/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
check_daemon_config() {
    local output
    if ! output=$("$@" 2>&1); then
        printf '%s\n' "$output" >&2
        return 1
    fi
    printf '%s\n' "$output"
    # FRR dry-run may exit successfully even when it cannot read the file.
    if printf '%s\n' "$output" | grep -Eq "failed to (open|read) configuration file|can't open configuration file"; then
        echo 'daemon configuration check failed' >&2
        return 1
    fi
}
check_daemon_config "$DEST/zebra" -u root -g root -C -f /etc/midr/zebra.conf
if [ "$MIDR_EDGE_BGP" = yes ]; then
    check_daemon_config "$DEST/bgpd" -u root -g root -S -C -f /etc/midr/bgpd.conf
fi
systemctl enable --now midr-logrotate.timer
systemctl enable --now midr-zebra.service
if [ "$MIDR_EDGE_BGP" = yes ]; then
    systemctl enable --now midr-bgpd.service
else
    systemctl disable --now midr-bgpd.service
fi
systemctl enable --now midrd.service
if [ "$MIDR_EDGE_BGP" = yes ]; then
    systemctl enable --now midr-bgp-prefix-provider.service
else
    systemctl disable --now midr-bgp-prefix-provider.service
fi
systemctl is-active midr-zebra.service midrd.service
if [ "$MIDR_EDGE_BGP" = yes ]; then
    systemctl is-active midr-bgpd.service midr-bgp-prefix-provider.service
fi
systemctl is-active midr-zebra.service midrd.service
zebra_pid=$(systemctl show -p MainPID --value midr-zebra.service)
midrd_pid=$(systemctl show -p MainPID --value midrd.service)
[ "$zebra_pid" -gt 0 ]
[ "$midrd_pid" -gt 0 ]
[ "$(readlink -f "/proc/$zebra_pid/exe")" = "$(readlink -f "$DEST/zebra")" ]
[ "$(readlink -f "/proc/$midrd_pid/exe")" = "$(readlink -f "$DEST/midrd")" ]
if [ "$MIDR_EDGE_BGP" = no ]; then
    if pgrep -x bgpd >/dev/null; then echo 'unexpected bgpd on internal MIDR' >&2; false; fi
fi
for file in "${PROTECTED_NETPLAN[@]}"; do
    cmp "$BACKUP/netplan/$file" "/etc/netplan/$file"
done
for attempt in $(seq 1 20); do
    if python3 "$DEST/midrd-vty.py" --socket /run/midr/midrd.vty \
        -c 'show midr self' -c 'show midr neighbors'; then
        break
    fi
    [ "$attempt" -lt 20 ] || exit 1
    sleep 1
done
install -d -m 755 /var/lib/midr
printf '%s\n' "$BACKUP" >/var/lib/midr/last-deploy-backup
ln -sfn "$DEST" /opt/midr-current
trap - ERR
printf 'DEPLOY PASS %s backup=%s\n' "$MIDR_NODE_NAME" "$BACKUP"
