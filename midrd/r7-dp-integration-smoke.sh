#!/usr/bin/env bash
# Three-node containerlab integration of group 2 (SPF/LS flooding) and
# group 3 (Zebra/FIB + GRE-ready data plane).
#
# Topology A - B - C with point-to-point links (10.77.x.x/30):
#   A(10.77.1.1) === B(10.77.1.2 / 10.77.2.1) === C(10.77.2.2)
#
# Each node runs the freshly built zebra on a private ZAPI socket and a midrd
# daemon pointed at it.  The MIDR prefixes (10.0.x.x/32) are also assigned on
# lo so that the proto-199 routes installed into the Linux FIB really forward:
# A reaches C's prefix through B and vice versa.
#
# Run as root on the docker host (containerlab + docker required).  Logs are
# kept under $MIDRD_INTEGRATION_LOG (default /tmp/midrd-dp-integration).
set -euo pipefail

IMAGE=${MIDRD_IMAGE:-frr-ubuntu24-ymy:init}
BUILD=${MIDRD_BUILD_CONTAINER:-frr-ubuntu24-ymy}
FRR_ROOT=${FRR_ROOT:-/home/frr/frr-midrd3}
RUNTIME=${MIDRD_INTEGRATION_LOG:-/tmp/midrd-dp-integration}
BASE_PORT=${MIDRD_INTEGRATION_BASE_PORT:-34001}
LAB=midr-dp-int
PROTO=199

PASS=0
FAIL=0

ok()  { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
hdr() { echo; echo "==== $* ===="; }

command -v containerlab >/dev/null 2>&1 || { echo 'containerlab is required' >&2; exit 2; }
command -v docker >/dev/null 2>&1 || { echo 'docker is required' >&2; exit 2; }

nexec() { timeout 15 docker exec "clab-${LAB}-$1" bash -c "$2"; }

# fetch_lib <soname>: stage a shared library used by the built binaries.
fetch_lib() {
	local name=$1 path real

	path=$(docker exec "$BUILD" bash -lc \
		"export LD_LIBRARY_PATH=$FRR_ROOT/lib/.libs; ldd $FRR_ROOT/zebra/.libs/zebra '$MIDRD_SRC' 2>/dev/null" \
		| awk -v lib="$name" '$1 == lib { print $3; exit }')
	[[ -n "$path" ]] || return 1
	real=$(docker exec "$BUILD" readlink -f "$path")
	[[ -n "$real" ]] || return 1
	docker cp "$BUILD:$real" "$RUNTIME/lib/$name" >/dev/null
}

prepare_artifacts() {
	local cand

	mkdir -p "$RUNTIME/bin" "$RUNTIME/lib"
	docker cp "$BUILD:$FRR_ROOT/zebra/.libs/zebra" "$RUNTIME/bin/zebra" >/dev/null
	MIDRD_SRC=
	for cand in /tmp/midrd-build-harness/midrd /tmp/midrd-build/midrd \
		    "$FRR_ROOT/midrd/.libs/midrd"; do
		if docker exec "$BUILD" test -x "$cand" 2>/dev/null; then
			MIDRD_SRC=$cand
			break
		fi
	done
	[[ -n "$MIDRD_SRC" ]] || {
		echo "no midrd binary found in $BUILD" >&2
		exit 2
	}
	docker cp "$BUILD:$MIDRD_SRC" "$RUNTIME/bin/midrd" >/dev/null
	fetch_lib libfrr.so.0 || { echo "unable to stage libfrr.so.0" >&2; exit 2; }
	fetch_lib libunwind.so.8 || true
	ln -sf libfrr.so.0 "$RUNTIME/lib/libfrr.so"
	echo "artifacts: midrd=$MIDRD_SRC zebra=$FRR_ROOT/zebra/.libs/zebra (in $RUNTIME)"
}

make_topology() {
	local topo=$1 a_listen=$2 b_listen=$3 c_listen=$4
	local a_peer=$5 b_peer=$6 b_to_c=$7 c_peer=$8
	local za='LD_LIBRARY_PATH=/opt/midrd-dp/lib /opt/midrd-dp/bin/zebra'

	cat >"$topo" <<EOF
name: $LAB
topology:
  nodes:
    a:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME/bin:/opt/midrd-dp/bin:ro
        - $RUNTIME/lib:/opt/midrd-dp/lib:ro
      cmd: sleep infinity
      exec:
        - sh -lc 'sysctl -w net.ipv4.ip_forward=1 || echo 1 > /proc/sys/net/ipv4/ip_forward'
        - ip addr add 10.77.1.1/30 dev eth1
        - ip addr add 10.0.1.1/32 dev lo
        - sh -lc 'printf "hostname a\nlog file /tmp/zebra.log\ndebug zebra kernel\n" >/tmp/zebra.conf; $za -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid -z /tmp/zserv.api --vty_socket /tmp -d'
        - sleep 1
        - sh -lc 'export LD_LIBRARY_PATH=/opt/midrd-dp/lib; exec /opt/midrd-dp/bin/midrd --node-id 101 --listen $a_listen --peer $a_peer --group 1 --prefix 10.0.1.1/32 --link 102:5 --link-addr 102:10.77.1.2 --zserv-path /tmp/zserv.api --takeover-delay 1500 --lifetime 3000 --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME/bin:/opt/midrd-dp/bin:ro
        - $RUNTIME/lib:/opt/midrd-dp/lib:ro
      cmd: sleep infinity
      exec:
        - sh -lc 'sysctl -w net.ipv4.ip_forward=1 || echo 1 > /proc/sys/net/ipv4/ip_forward'
        - ip addr add 10.77.1.2/30 dev eth1
        - ip addr add 10.77.2.1/30 dev eth2
        - ip addr add 10.0.2.2/32 dev lo
        - sh -lc 'printf "hostname b\nlog file /tmp/zebra.log\ndebug zebra kernel\n" >/tmp/zebra.conf; $za -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid -z /tmp/zserv.api --vty_socket /tmp -d'
        - sleep 1
        - sh -lc 'export LD_LIBRARY_PATH=/opt/midrd-dp/lib; exec /opt/midrd-dp/bin/midrd --node-id 102 --listen $b_listen --peer $b_peer --peer $b_to_c --group 1 --prefix 10.0.2.2/32 --link 101:5 --link 103:7 --link-addr 101:10.77.1.1 --link-addr 103:10.77.2.2 --zserv-path /tmp/zserv.api --takeover-delay 1500 --lifetime 3000 --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME/bin:/opt/midrd-dp/bin:ro
        - $RUNTIME/lib:/opt/midrd-dp/lib:ro
      cmd: sleep infinity
      exec:
        - sh -lc 'sysctl -w net.ipv4.ip_forward=1 || echo 1 > /proc/sys/net/ipv4/ip_forward'
        - ip addr add 10.77.2.2/30 dev eth1
        - ip addr add 10.0.3.3/32 dev lo
        - sh -lc 'printf "hostname c\nlog file /tmp/zebra.log\ndebug zebra kernel\n" >/tmp/zebra.conf; $za -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid -z /tmp/zserv.api --vty_socket /tmp -d'
        - sleep 1
        - sh -lc 'export LD_LIBRARY_PATH=/opt/midrd-dp/lib; exec /opt/midrd-dp/bin/midrd --node-id 103 --listen $c_listen --peer $c_peer --group 1 --prefix 10.0.3.3/32 --link 102:7 --link-addr 102:10.77.2.1 --zserv-path /tmp/zserv.api --takeover-delay 1500 --lifetime 3000 --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF
}

wait_node_log() {
	local n=$1 pattern=$2 timeout=$3
	local i

	for ((i = 0; i < timeout * 10; i++)); do
		if nexec "$n" "grep -Eq '$pattern' /tmp/midrd.log" 2>/dev/null; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

node_fib() {
	nexec "$1" "ip route show proto $PROTO" 2>/dev/null || true
	nexec "$1" "ip -6 route show proto $PROTO" 2>/dev/null || true
}
node_fib_has() { node_fib "$1" | grep -F -- "$2" >/dev/null 2>&1; }

# One docker exec per check: the node resolves inline ("via N") and nexthop
# group ("nhid N") forms itself, so no nested exec can block the script.
node_fib_check() {
	# <node> <prefix> <nexthop>
	nexec "$1" "
		line=\$(ip route show proto $PROTO | grep -F -- '$2' | head -1)
		if [ -z \"\$line\" ]; then
			line=\$(ip -6 route show proto $PROTO | grep -F -- '$2' | head -1)
		fi
		[ -n \"\$line\" ] || exit 1
		case \"\$line\" in *'$3'*) exit 0 ;; esac
		nhid=\$(printf '%s' \"\$line\" | grep -oE 'nhid [0-9]+' | awk '{print \$2}')
		[ -n \"\$nhid\" ] || exit 1
		grp=\$(ip nexthop show id \"\$nhid\" 2>/dev/null |
			grep -oE 'group [0-9,]+' | sed 's/group //' | tr ',' ' ')
		if [ -z \"\$grp\" ]; then
			ip nexthop show id \"\$nhid\" 2>/dev/null | grep -qF -- '$3'
			exit \$?
		fi
		for id in \$grp; do
			if ip nexthop show id \"\$id\" 2>/dev/null | grep -qF -- '$3'; then
				exit 0
			fi
		done
		exit 1
	" 2>/dev/null
}

node_fib_nh() { node_fib_check "$1" "$2" "$3"; }

# The SPF log line appears as soon as the adapter staged the batch; the route
# reaches the Linux FIB only after the 100 ms deferred submit, the ZAPI round
# trip and zebra's nexthop resolution.  Wait for the FIB instead of assuming it
# is instant.
wait_fib_nh() {
	local n=$1 prefix=$2 nh=$3 timeout=${4:-20}
	local i

	for ((i = 0; i < timeout * 4; i++)); do
		if node_fib_nh "$n" "$prefix" "$nh"; then
			if ((i > 0)); then
				echo "    (node $n $prefix ready after ~$(awk -v i=$i 'BEGIN{printf "%.2f", i*0.25}')s)"
			fi
			return 0
		fi
		sleep 0.25
	done
	return 1
}

nexec_cat() { nexec "$1" "cat /tmp/midrd.log" 2>/dev/null || true; }

RUN="$RUNTIME/run"
rm -rf "$RUN"
mkdir -p "$RUN"
TOPO="$RUN/topology.clab.yaml"

echo "=== MIDR group-3 three-node containerlab integration ==="
echo "image=$IMAGE lab=$LAB runtime=$RUNTIME"

prepare_artifacts
make_topology "$TOPO" \
	"10.77.1.1:$BASE_PORT" "0.0.0.0:$((BASE_PORT + 1))" \
	"10.77.2.2:$((BASE_PORT + 2))" \
	"10.77.1.2:$((BASE_PORT + 1))" "10.77.1.1:$BASE_PORT" \
	"10.77.2.2:$((BASE_PORT + 2))" "10.77.2.1:$((BASE_PORT + 1))"

cleanup_case() { containerlab destroy --topo "$TOPO" --cleanup >/dev/null 2>&1 || true; }
trap cleanup_case EXIT

hdr "1. deploy and wait for SPF convergence"
if containerlab deploy --topo "$TOPO" --reconfigure >/dev/null 2>&1; then
	ok "containerlab deploy"
else
	bad "containerlab deploy failed"
	containerlab destroy --topo "$TOPO" --cleanup >/dev/null 2>&1 || true
	exit 1
fi

converged=1
for n in a b c; do
	if wait_node_log "$n" 'spf generation=.* routes=3' 60; then
		ok "node $n: spf generation routes=3"
	else
		bad "node $n: no spf generation routes=3"
		converged=0
	fi
done
sleep 1
for n in a b c; do nexec_cat "$n" >"$RUN/$n.log"; done
if [[ "$converged" != 1 ]]; then
	for n in a b c; do
		echo "--- $n" >&2
		tail -30 "$RUN/$n.log" >&2
	done
	exit 1
fi

hdr "2. proto-$PROTO FIB per node matches the SPF expectation"
# node a reaches b and c through b (10.77.1.2)
for spec in "a 10.0.2.2 10.77.1.2" "a 10.0.3.3 10.77.1.2" \
	    "b 10.0.1.1 10.77.1.1" "b 10.0.3.3 10.77.2.2" \
	    "c 10.0.1.1 10.77.2.1" "c 10.0.2.2 10.77.2.1"; do
	set -- $spec
	if wait_fib_nh "$1" "$2" "$3" 20; then
		ok "node $1: route $2 via $3"
	else
		bad "node $1: missing route $2 via $3"
		echo "--- node $1 proto-$PROTO FIB:" >&2
		node_fib "$1" >&2 || true
	fi
done

hdr "3. node b forwards towards c"
if wait_fib_nh b 10.0.3.3 10.77.2.2 20; then
	ok "node b: c prefix 10.0.3.3 via 10.77.2.2"
else
	bad "node b: no route towards c prefix"
	node_fib b >&2 || true
fi

hdr "4. true end-to-end forwarding over the MIDR path"
# The ping source must be the node's own MIDR prefix: the link address is not
# routable from the far node, so a transit reply would fall back to the
# management default route.  The source is bound so the packet cannot take the
# management shortcut, and the route lookup must resolve over the data link.
ping_midr_path() {
	local node=$1 src=$2 dev=$3 target=$4 label=$5
	local i route_get

	for ((i = 0; i < 5; i++)); do
		route_get=$(nexec "$node" "ip route get $target from $src" 2>/dev/null ||
			true)
		if [[ $route_get == *"dev $dev"* ]] &&
			nexec "$node" "ping -c 3 -W 2 -I $src $target" >/dev/null 2>&1; then
			ok "ping $label (source $src over $dev)"
			return 0
		fi
		sleep 1
	done
	bad "ping $label failed over the MIDR path"
	echo "    route get $target from $src: ${route_get:-<none>}" >&2
	nexec "$node" "ip route show proto $PROTO" >&2 || true
	nexec "$node" "ip nexthop show" >&2 || true
	return 1
}

ping_midr_path a 10.0.1.1 eth1 10.0.3.3 "a -> 10.0.3.3 (c prefix over b)" || true
ping_midr_path c 10.0.3.3 eth1 10.0.1.1 "c -> 10.0.1.1 (a prefix over b)" || true

hdr "5. destroy leaves nothing behind"
containerlab destroy --topo "$TOPO" --cleanup >/dev/null 2>&1 || true
leftover=$(docker ps -a --filter "name=clab-${LAB}-" --format '{{.Names}}' 2>/dev/null || true)
if [[ -z "$leftover" ]]; then
	ok "no clab-$LAB containers left"
else
	bad "leftover containers: $leftover"
fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL (logs: $RUN) ==="
[[ "$FAIL" -eq 0 ]]
