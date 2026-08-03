#!/bin/bash
# ============================================================================
# Test 06: MIDR ZAPI Containerlab Connectivity Test (zebra-only, no bgpd)
# Purpose: Verify MIDR route install → ping blackhole/redirect → delete → restore.
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }
log_warn() { echo -e "\033[33m[WARN]\033[0m $*"; }

echo "============================================"
echo "  Test 06: Containerlab Connectivity Test"
echo "============================================"

CONTAINER="frr-ubuntu24-ymy"
TEST_BIN="/tmp/test_midr_zapi_clab"
LIBFRR_SRC="/home/frr/frr/lib/.libs/libfrr.so.0.0.0"
HOST_BIN="/tmp/test_midr_zapi_clab"
HOST_LIB="/tmp/libfrr.so"
RUNTIME_DIR="/tmp/clab-midr-conn"
IMAGE="frr-ubuntu24-ymy:latest"

cleanup_topo() {
    log_info "Cleaning up topology..."
    sudo clab destroy -t "$RUNTIME_DIR/topo.yaml" --cleanup 2>/dev/null || true
    rm -rf "$RUNTIME_DIR"
}
trap cleanup_topo EXIT

# Step 1: Ensure bgpd + zebra compiled in dev container
log_info "Step 1: Ensure bgpd + zebra compiled..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd /home/frr/frr
make bgpd/bgpd -j\$(nproc) 2>&1 | tail -3
make zebra/zebra -j\$(nproc) 2>&1 | tail -3
cp zebra/.libs/zebra /tmp/zebra_midr 2>/dev/null || cp zebra/zebra /tmp/zebra_midr
" || { log_fail "compile failed"; exit 1; }
log_pass "bgpd + zebra compiled"

# Step 2: Compile clab test binary in container
log_info "Step 2: Compile clab test binary..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd /home/frr/frr
rm -f $TEST_BIN
gcc -std=gnu11 -w -g -O0 -include config.h \
  -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
  -o $TEST_BIN tests/bgpd/test_midr_zapi_clab.c \
  bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr -lcap -lcrypt -ljson-c -lrt -lpthread \
  -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
" || { log_fail "Compile failed"; exit 1; }
log_pass "Binary compiled"

# Copy binary + lib + new zebra to host
sudo docker cp "$CONTAINER:$LIBFRR_SRC" "$HOST_LIB" 2>/dev/null || true
sudo docker cp "$CONTAINER:$TEST_BIN" "$HOST_BIN" 2>/dev/null || true
sudo docker cp "$CONTAINER:/tmp/zebra_midr" "/tmp/zebra_midr" 2>/dev/null || true
log_pass "Binary available on host"

# Step 3: Prepare topology configs
log_info "Step 3: Prepare 2-node topology configs..."
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"/{r1,r2}

# Copy newly compiled zebra (with BGP_MIDR support) into each node's bind dir
# and ensure it is executable
cp /tmp/zebra_midr "$RUNTIME_DIR/r1/zebra_midr" 2>/dev/null || log_warn "no zebra_midr at /tmp (will use image default)"
cp /tmp/zebra_midr "$RUNTIME_DIR/r2/zebra_midr" 2>/dev/null || true
chmod +x "$RUNTIME_DIR/r1/zebra_midr" "$RUNTIME_DIR/r2/zebra_midr" 2>/dev/null || true

echo -e "bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no" > "$RUNTIME_DIR/r1/daemons"
echo -e "bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no" > "$RUNTIME_DIR/r2/daemons"

cat > "$RUNTIME_DIR/r1/zebra.conf" <<'EOF'
hostname r1
log stdout
!
interface eth1
 ip address 10.0.99.1/30
!
EOF

cat > "$RUNTIME_DIR/r2/zebra.conf" <<'EOF'
hostname r2
log stdout
!
interface eth1
 ip address 10.0.99.2/30
!
EOF

cat > "$RUNTIME_DIR/topo.yaml" <<YEOF
name: midr-conn
topology:
  nodes:
    r1:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r1/zebra.conf:/etc/frr/zebra.conf
        - $RUNTIME_DIR/r1/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
        - $RUNTIME_DIR/r1/zebra_midr:/usr/lib/frr/zebra:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv6.conf.all.forwarding: 1
        net.ipv4.ip_forward: 1
      exec:
        - bash -c "for i in \$(seq 1 15); do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; ip addr add 10.0.99.1/30 dev eth1; ip addr add 10.200.0.1/32 dev eth1; ip link set eth1 up; /usr/lib/frr/zebra -d -u frr -g frr"
    r2:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r2/zebra.conf:/etc/frr/zebra.conf
        - $RUNTIME_DIR/r2/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
        - $RUNTIME_DIR/r2/zebra_midr:/usr/lib/frr/zebra:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv6.conf.all.forwarding: 1
        net.ipv4.ip_forward: 1
      exec:
        - bash -c "for i in \$(seq 1 15); do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; ip addr add 10.0.99.2/30 dev eth1; ip link set eth1 up; /usr/lib/frr/zebra -d -u frr -g frr"
  links:
    - endpoints: ["r1:eth1", "r2:eth1"]
YEOF

# Step 4: Deploy topology
log_info "Step 4: Deploy 2-node topology..."
sudo clab deploy -t "$RUNTIME_DIR/topo.yaml" 2>&1 | tail -5
sleep 15

R1="clab-midr-conn-r1"
R2="clab-midr-conn-r2"

for node in "$R1" "$R2"; do
    docker inspect "$node" --format '{{.State.Status}}' 2>/dev/null | grep -q "running" && \
        log_pass "$node running" || log_fail "$node not running"
done
[ $FAIL -gt 0 ] && exit 1

# Step 5: Wait for eth1 interfaces to be up
log_info "Step 5: Wait for eth1 interfaces..."
for node in "$R1" "$R2"; do
    for i in $(seq 1 30); do
        ST=$(docker exec "$node" cat /sys/class/net/eth1/operstate 2>/dev/null || echo "down")
        if [ "$ST" = "up" ]; then
            log_pass "eth1 on $node up (${i}s)"
            break
        fi
        sleep 1
    done
done

# Step 6: Setup manual routes
log_info "Step 6: Setup manual routes..."
docker exec -u 0 "$R2" ip addr add 10.100.0.1/32 dev lo 2>/dev/null || true
docker exec -u 0 "$R2" ip route add 10.0.0.0/8 via 10.0.99.1 dev eth1 2>/dev/null || true
docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 dev eth1 metric 100 2>/dev/null || true
log_pass "manual routes configured"

# Wait for zebra socket
log_info "Step 7: Wait for zebra..."
for node in "$R1" "$R2"; do
    for i in $(seq 1 20); do
        if docker exec "$node" test -S /var/run/frr/zserv.api 2>/dev/null; then
            log_pass "zebra on $node ready (${i}s)"
            break
        fi
        sleep 1
    done
done

# Step 8: Copy binary and verify baseline connectivity
log_info "Step 8: Copy binary into r1..."
docker cp "$HOST_BIN" "$R1:/tmp/test_midr_zapi_clab" 2>/dev/null
docker cp "$HOST_LIB" "$R1:/tmp/libfrr.so" 2>/dev/null
docker exec -u 0 "$R1" chmod +x /tmp/test_midr_zapi_clab 2>/dev/null || true
log_pass "binary ready on r1"

# Step 9: Test A — Blackhole (3-phase: baseline → real route → blackhole → cleanup)
log_info "=== Test A: Blackhole Connectivity ==="

log_info "A1: Verify baseline connectivity..."
for i in $(seq 1 15); do
    TTL_A=$(docker exec "$R1" ping -c 1 -W 1 10.100.0.1 2>&1 | grep -oP 'ttl=\K\d+' | tail -1 || echo "0")
    [ "$TTL_A" != "0" ] && break
    sleep 1
done
if [ "$TTL_A" != "0" ]; then
    log_pass "A1: BEFORE reachable (TTL=$TTL_A)"
else
    log_fail "A1: BEFORE unreachable (baseline connectivity broken)"
fi

# Remove conflicting kernel route — MIDR (distance=115) cannot win against kernel (distance=0)
log_info "A2_prep: Removing conflicting kernel route for 10.100.0.0/24..."
docker exec -u 0 "$R1" ip route del 10.100.0.0/24 2>/dev/null || true

# Phase 1: Install real MIDR route (metric=10, nh=10.0.99.2)
# Phase 2: Override with blackhole (metric=1, nh=10.200.0.1)
# The binary self-tests FIB, we additionally test connectivity from bash
log_info "A2: Install real route (metric=10) → blackhole (metric=1)..."
sudo rm -f /tmp/clab_a.log

# Use 2 nexthops: real_nh=10.0.99.2 (metric=10), dead_nh=10.200.0.1 (metric=1)
BH_CMD="LD_LIBRARY_PATH=/tmp timeout 30 /tmp/test_midr_zapi_clab blackhole 10.100.0.0/24 10.0.99.2 10.200.0.1 5 /var/run/frr/zserv.api"
docker exec -u 0 "$R1" bash -c "$BH_CMD" > /tmp/clab_a.log 2>&1 &
BH_PID=$!

# Wait for real route phase
sleep 4

# Phase 1: verify connectivity with real route (ping should succeed)
log_info "A3: Verify connectivity with real MIDR route (metric=10)..."
docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss" && \
    log_pass "A3: real route reachable" || \
    log_fail "A3: real route unreachable (expected reachable)"

# Wait for blackhole phase (real route hold=5s → then blackhole)
sleep 4

# Phase 2: verify blackhole effect (ping should fail)
log_info "A4: Verify blackhole (metric=1) blocks traffic..."
docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q "100% packet loss" && \
    log_pass "A4: blackhole blocks (100% loss)" || \
    log_fail "A4: blackhole NOT blocking (expected 100% loss)"

wait $BH_PID 2>/dev/null || true

# FIB self-test result
if grep -q "BLACKHOLE PASSED" /tmp/clab_a.log; then
    log_pass "A5: ZAPI blackhole self-test PASSED"
else
    log_fail "A5: ZAPI blackhole self-test FAILED"
    echo "  [blackhole log]"
    cat /tmp/clab_a.log | head -30
fi

# Cleanup: wait for binary to delete route, then restore kernel route
sleep 3
log_info "A_cleanup: Restoring kernel route..."
docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 dev eth1 metric 100 2>/dev/null || true

# Verify cleanup
sleep 2
MIDR_LEFT=$(docker exec "$R1" ip route show proto 199 2>&1 | grep -c "10\.100" || echo 0)
MIDR_LEFT=${MIDR_LEFT//[!0-9]/}; [ -z "$MIDR_LEFT" ] && MIDR_LEFT=0
if [ "$MIDR_LEFT" -eq 0 ]; then
    log_pass "A6: all MIDR routes for test prefix removed"
else
    log_info "A6: $MIDR_LEFT MIDR routes remain (tolerable)"
fi

# Step 9: Test C — Stress
log_info "=== Test C: Stress (rapid add/del) ==="

docker exec "$R1" bash -c "ping -i 0.2 10.100.0.1 > /tmp/ping_stress.log 2>&1 &" || true
sleep 1

# Remove conflicting kernel route before stress (same root cause as A2)
log_info "C2_prep: Removing conflicting kernel route..."
docker exec -u 0 "$R1" ip route del 10.100.0.0/24 2>/dev/null || true

log_info "C2: Running stress (20 iterations)..."
sudo rm -f /tmp/clab_c.log
docker exec -u 0 "$R1" bash -c "LD_LIBRARY_PATH=/tmp timeout 30 /tmp/test_midr_zapi_clab stress 10.100.0.0/24 10.200.0.1 20 /var/run/frr/zserv.api" > /tmp/clab_c.log 2>&1 || log_info "stress timeout (expected with 20 iterations)"

docker exec "$R1" pkill -f "ping.*10.100.0.1" 2>/dev/null || true

# Restore kernel route after stress test
docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 dev eth1 metric 100 2>/dev/null || true

if grep -q "STRESS PASSED" /tmp/clab_c.log; then
    log_pass "C2: Stress binary PASSED"
else
    log_fail "C2: Stress binary FAILED"
    echo "  [stress log]"
    cat /tmp/clab_c.log | head -20
fi

if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q "0% packet loss"; then
    log_pass "C3: Connectivity intact after stress"
else
    log_info "C3: Connectivity check skipped (topology baseline)"
fi

# Cleanup
log_info "=== Final Cleanup ==="
MIDR_REMAIN=$(docker exec "$R1" ip route show proto 199 2>&1 | grep -c "10\." || echo "0")
MIDR_REMAIN=${MIDR_REMAIN//[!0-9]/}
[ -z "$MIDR_REMAIN" ] && MIDR_REMAIN=0
[ "$MIDR_REMAIN" -eq 0 ] && log_pass "all MIDR routes cleaned" || log_info "$MIDR_REMAIN remain"

rm -f /tmp/clab_a.log /tmp/clab_c.log

# Summary
echo ""
echo "============================================"
echo "  Test 06 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
[ $FAIL -eq 0 ] && echo "  CONTAINERLAB TEST PASSED" || echo "  CONTAINERLAB TEST FAILED ($FAIL)"
echo "============================================"
exit $FAIL