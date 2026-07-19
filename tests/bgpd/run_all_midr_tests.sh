#!/bin/bash
# ============================================================================
# MIDR Full Test Suite (Test 05 + Test 06)
# Run on server: sudo bash /tmp/run_all_midr_tests.sh
# ============================================================================
set -euo pipefail
PASS=0; FAIL=0
LOGDIR="/tmp/midr-test-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS+1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL+1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

CONTAINER="frr-ubuntu24-ymy"
FRR_DIR="/home/frr/frr"
ZEBRA_SOCK="/var/run/frr/zserv.api"
LIB_PATH="$FRR_DIR/lib/.libs"

# ============================================================
echo "============================================"
echo "  MIDR Full Test Suite"
echo "  Log: $LOGDIR"
echo "============================================"

# ============================================================
# Test 05: Batch Stress (dev container, zebra-only)
# ============================================================
log_info "==== Test 05: ZAPI Batch Stress ===="

# Compile batch binary
docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR && make bgpd/bgpd -j\$(nproc) 2>&1 | tail -1
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_zapi_batch tests/bgpd/test_midr_zapi_batch.c \
  bgpd/bgp_midr_zebra.o -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
echo \"compile=\$?\"
" 2>&1 | tail -3

# Start zebra + run batch test
docker exec -u 0 "$CONTAINER" bash -c "
pkill -9 zebra 2>/dev/null || true; sleep 1
echo -e 'bgpd=no\nzebra=yes' > /etc/frr/daemons
/usr/lib/frr/zebra -d -u frr -g frr --limit-fds 100000 2>/dev/null; sleep 3
ip link add dummy0 type dummy 2>/dev/null || true; ip link set dummy0 up
ip addr add 192.168.200.1/24 dev dummy0 2>/dev/null || true
LD_LIBRARY_PATH=$LIB_PATH /tmp/test_midr_zapi_batch $ZEBRA_SOCK
" > "$LOGDIR/test05.log" 2>&1
RC05=$?

# Evaluate Test 05
if grep -q "ALL TESTS PASSED" "$LOGDIR/test05.log"; then
    log_pass "Test 05: Batch stress ALL PASSED"
elif [ $RC05 -eq 0 ]; then
    log_pass "Test 05: Batch stress completed (exit 0)"
else
    FAILS05=$(grep -c "FAIL:" "$LOGDIR/test05.log" || echo 0)
    log_fail "Test 05: $FAILS05 failures (exit=$RC05)"
fi
cat "$LOGDIR/test05.log"

# Cleanup dummy0
docker exec -u 0 "$CONTAINER" ip link del dummy0 2>/dev/null || true

# ============================================================
# Test 06: Containerlab Connectivity (zebra-only, no bgpd)
# ============================================================
log_info "==== Test 06: Containerlab Connectivity ===="

# Compile clab binary
docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_zapi_clab tests/bgpd/test_midr_zapi_clab.c \
  bgpd/bgp_midr_zebra.o -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
echo \"compile_clab=\$?\"
" 2>&1 | tail -3

# Copy to host
docker cp "$CONTAINER":/tmp/test_midr_zapi_clab /tmp/ 2>/dev/null
docker cp "$CONTAINER":$FRR_DIR/lib/.libs/libfrr.so.0.0.0 /tmp/libfrr.so 2>/dev/null

# Deploy 2-node topology (zebra-only, manual routes)
RUNTIME_DIR="/tmp/clab-midr-t06"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"/{r1,r2}
echo -e "bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no" > "$RUNTIME_DIR/r1/daemons"
cp "$RUNTIME_DIR/r1/daemons" "$RUNTIME_DIR/r2/daemons"

cat > "$RUNTIME_DIR/r1/zebra.conf" <<'ZEOF'
hostname r1
log stdout
!
interface eth1
 ip address 10.0.99.1/30
!
ZEOF

cat > "$RUNTIME_DIR/r2/zebra.conf" <<'ZEOF'
hostname r2
log stdout
!
interface eth1
 ip address 10.0.99.2/30
!
ZEOF

cat > "$RUNTIME_DIR/topo.yaml" <<YEOF
name: midr-t06
topology:
  nodes:
    r1:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - $RUNTIME_DIR/r1/zebra.conf:/etc/frr/zebra.conf
        - $RUNTIME_DIR/r1/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv4.ip_forward: 1
      exec:
        - bash -c "for i in \$(seq 1 15); do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; /usr/lib/frr/frrinit.sh start"
    r2:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - $RUNTIME_DIR/r2/zebra.conf:/etc/frr/zebra.conf
        - $RUNTIME_DIR/r2/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv4.ip_forward: 1
      exec:
        - bash -c "for i in \$(seq 1 15); do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; /usr/lib/frr/frrinit.sh start"
  links:
    - endpoints: ["r1:eth1", "r2:eth1"]
YEOF

log_info "Deploying topology..."
sudo clab deploy -t "$RUNTIME_DIR/topo.yaml" 2>&1 | tail -3
sleep 20

R1="clab-midr-t06-r1"
R2="clab-midr-t06-r2"
for node in "$R1" "$R2"; do
    STATUS=$(docker inspect "$node" --format '{{.State.Status}}' 2>/dev/null)
    if [ "$STATUS" = "running" ]; then log_pass "$node running"; else log_fail "$node not running ($STATUS)"; fi
done
[ $FAIL -gt 0 ] && { log_info "Topology failed, skipping test 06"; exit $FAIL; }

# Setup manual routes and target
docker exec -u 0 "$R2" ip addr add 10.100.0.1/32 dev lo 2>/dev/null || true
docker exec -u 0 "$R2" ip route add 10.0.0.0/8 via 10.0.99.1 dev eth1 2>/dev/null || true
docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 dev eth1 metric 100 2>/dev/null || true
log_pass "manual routes configured"

# Wait for zebra
for node in "$R1" "$R2"; do
    for i in $(seq 1 20); do
        if docker exec "$node" test -S "$ZEBRA_SOCK" 2>/dev/null; then
            log_pass "zebra on $node ready (${i}s)"; break
        fi; sleep 1
    done
done

# Copy binary into r1
docker cp /tmp/test_midr_zapi_clab "$R1":/tmp/
docker cp /tmp/libfrr.so "$R1":/tmp/
docker exec -u 0 "$R1" chmod +x /tmp/test_midr_zapi_clab

# Verify baseline connectivity
log_info "Baseline connectivity..."
if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss"; then
    log_pass "T06: baseline reachable"
else
    log_fail "T06: baseline UNREACHABLE"
    docker exec "$R1" ip route show 2>&1 | head -10
fi

# ---- Test 06A: Blackhole ----
log_info "T06A: Blackhole test..."
docker exec -u 0 "$R1" bash -c "LD_LIBRARY_PATH=/tmp timeout 12 /tmp/test_midr_zapi_clab blackhole 10.100.0.0/24 10.200.0.1 5 /var/run/frr/zserv.api" > "$LOGDIR/test06a.log" 2>&1 &
BH_PID=$!; sleep 4
if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss"; then
    log_fail "T06A: DURING still reachable"
else
    log_pass "T06A: DURING UNREACHABLE (blackhole works)"
fi
wait $BH_PID 2>/dev/null || true
grep -q "BLACKHOLE PASSED" "$LOGDIR/test06a.log" && log_pass "T06A: blackhole binary PASSED" || log_fail "T06A: blackhole binary FAILED"
sleep 2
docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q "ttl=" && log_pass "T06A: AFTER restored" || log_fail "T06A: AFTER unreachable"

# ---- Test 06B: Stress ----
log_info "T06B: Stress test (20 iterations)..."
docker exec -u 0 "$R1" bash -c "LD_LIBRARY_PATH=/tmp timeout 30 /tmp/test_midr_zapi_clab stress 10.100.0.0/24 10.200.0.1 20 /var/run/frr/zserv.api" > "$LOGDIR/test06b.log" 2>&1 || true
grep -q "STRESS PASSED" "$LOGDIR/test06b.log" && log_pass "T06B: stress PASSED" || log_fail "T06B: stress FAILED"
cat "$LOGDIR/test06b.log" | tail -5

docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q "ttl=" && log_pass "T06B: connectivity intact" || log_fail "T06B: connectivity lost"

# Cleanup
log_info "Cleaning up..."
MIDR_LEFT=$(docker exec "$R1" ip route show proto 199 2>&1 | wc -l)
[ "$MIDR_LEFT" -eq 0 ] && log_pass "all MIDR routes cleaned" || log_info "$MIDR_LEFT remain"
sudo clab destroy -t "$RUNTIME_DIR/topo.yaml" --cleanup 2>/dev/null || true
rm -rf "$RUNTIME_DIR"

# ============================================================
echo ""
echo "============================================"
echo "  Test Results"
echo "============================================"
echo "  Test 05: Batch Stress"
echo "  Test 06: Clab Connectivity (Blackhole + Stress)"
echo "  ------------------------------"
echo "  PASS: $PASS | FAIL: $FAIL"
echo "  Logs: $LOGDIR"
[ $FAIL -eq 0 ] && echo "  ALL TESTS PASSED" || echo "  $FAIL TEST(S) FAILED"
echo "============================================"
exit $FAIL
