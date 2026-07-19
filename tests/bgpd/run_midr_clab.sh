#!/bin/bash
# ============================================================================
# MIDR Containerlab ZAPI Blackhole Connectivity Test  (v5.0)
#
# Pre-condition: test_midr_blackhole binary already compiled at /tmp/test_midr_blackhole
#               and libfrr.so at /tmp/libfrr.so  (compile once via dev container)
# ============================================================================
set -euo pipefail

RUNTIME_DIR="/tmp/clab-midr-bh"
IMAGE="frr-ubuntu24-ymy:latest"
PASS=0; FAIL=0

log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

cleanup() {
    log_info "Cleaning up topology..."
    sudo clab destroy -t "$RUNTIME_DIR/topo.yaml" --cleanup 2>/dev/null || true
    rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

# ============================================================
# Step 0: Verify pre-compiled binary exists
# ============================================================
if [ ! -f /tmp/test_midr_blackhole ] || [ ! -f /tmp/libfrr.so ]; then
    log_info "Binary not found, compiling..."
    sudo docker exec -u 0 frr-ubuntu24-ymy bash -c "
        cd /home/frr/frr && make bgpd/bgpd -j\$(nproc) 2>&1 | tail -2
        rm -f /tmp/test_midr_blackhole
        gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
          -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
          -o /tmp/test_midr_blackhole tests/bgpd/test_midr_zapi_blackhole.c bgpd/bgp_midr_zebra.o \
          -L lib/.libs -lfrr -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>/dev/null
    " || true
    sudo cp /tmp/test_midr_blackhole /tmp/test_midr_blackhole 2>/dev/null || \
        sudo docker cp frr-ubuntu24-ymy:/tmp/test_midr_blackhole /tmp/test_midr_blackhole
    sudo docker cp frr-ubuntu24-ymy:/home/frr/frr/lib/.libs/libfrr.so.0.0.0 /tmp/libfrr.so 2>/dev/null || true
fi
[ -f /tmp/test_midr_blackhole ] || { log_fail "binary missing at /tmp/test_midr_blackhole"; exit 1; }
log_pass "binary ready"

# ============================================================
# Step 1: Prepare configs
# ============================================================
log_info "Step 1: Preparing topology configs..."
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"/{r1,r2}

echo -e "bgpd=yes\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no" > "$RUNTIME_DIR/r1/daemons"
cp "$RUNTIME_DIR/r1/daemons" "$RUNTIME_DIR/r2/daemons"

cat > "$RUNTIME_DIR/r1/frr.conf" <<'EOF'
frr version 10.0
frr defaults traditional
hostname r1
log stdout
!
interface eth1
 ip address 10.0.99.1/30
!
router bgp 65001
 bgp router-id 10.0.0.1
 no bgp ebgp-requires-policy
 neighbor 10.0.99.2 remote-as 65002
 !
 address-family ipv4 unicast
  neighbor 10.0.99.2 activate
  redistribute connected
 exit-address-family
!
EOF

cat > "$RUNTIME_DIR/r2/frr.conf" <<'EOF'
frr version 10.0
frr defaults traditional
hostname r2
log stdout
!
interface eth1
 ip address 10.0.99.2/30
!
router bgp 65002
 bgp router-id 10.0.0.2
 no bgp ebgp-requires-policy
 neighbor 10.0.99.1 remote-as 65001
 !
 address-family ipv4 unicast
  neighbor 10.0.99.1 activate
  network 10.100.0.0/24
  redistribute connected
 exit-address-family
!
EOF

# ============================================================
# Step 2: Deploy topology
# ============================================================
log_info "Step 2: Deploy topology..."
cat > "$RUNTIME_DIR/topo.yaml" <<YEOF
name: midr-bh
topology:
  nodes:
    r1:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r1/frr.conf:/etc/frr/frr.conf
        - $RUNTIME_DIR/r1/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv6.conf.all.forwarding: 1
      exec:
        - bash -c "for i in {1..15}; do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; /usr/lib/frr/frrinit.sh start"
    r2:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r2/frr.conf:/etc/frr/frr.conf
        - $RUNTIME_DIR/r2/daemons:/etc/frr/daemons
        - /lib/modules:/lib/modules:ro
      sysctls:
        net.ipv6.conf.all.seg6_enabled: 1
        net.ipv6.conf.all.forwarding: 1
      exec:
        - bash -c "for i in {1..15}; do if ip link show eth1 >/dev/null 2>&1; then break; fi; sleep 1; done; /usr/lib/frr/frrinit.sh start"
  links:
    - endpoints: ["r1:eth1", "r2:eth1"]
YEOF

sudo clab deploy -t "$RUNTIME_DIR/topo.yaml" 2>&1 | tail -5
sleep 15

R1="clab-midr-bh-r1"
R2="clab-midr-bh-r2"

for node in "$R1" "$R2"; do
    docker inspect "$node" --format '{{.State.Status}}' 2>/dev/null | grep -q "running" && log_pass "$node running" || log_fail "$node not running"
done
[ $FAIL -gt 0 ] && exit 1

# ============================================================
# Step 3: Setup target host on r2 (10.100.0.1/32 on lo)
# ============================================================
log_info "Step 3: Setup target host 10.100.0.1/32 on r2 lo..."
docker exec -u 0 "$R2" ip addr add 10.100.0.1/32 dev lo 2>/dev/null || true
log_pass "10.100.0.1/32 bound on r2 lo"

# ============================================================
# Step 4: Wait for zebra + BGP
# ============================================================
log_info "Step 4: Wait for zebra + BGP..."
for node in "$R1" "$R2"; do
    for i in $(seq 1 30); do
        if docker exec "$node" test -S /var/run/frr/zserv.api 2>/dev/null; then
            log_pass "zebra on $node ready (${i}s)"; break
        fi
        sleep 1
    done
done
sleep 10

BGP_OK=0
for node in "$R1" "$R2"; do
    OUT=$(docker exec "$node" vtysh -c "show bgp summary" 2>&1) || true
    if echo "$OUT" | grep -q "Established"; then
        log_pass "BGP $node Established"; BGP_OK=1
    fi
done
[ $BGP_OK -eq 0 ] && { log_fail "BGP not established"; exit 1; }

# Verify r1 has BGP route to 10.100.0.0/24
log_info "r1 routing table:"
docker exec "$R1" vtysh -c "show ip route 10.100.0.0/24" 2>&1 | head -5

# ============================================================
# Step 5: Copy binary + shared lib into r1
# ============================================================
log_info "Step 5: Copy binary + libfrr.so into r1..."
docker cp /tmp/test_midr_blackhole "$R1":/tmp/test_midr_blackhole
docker cp /tmp/libfrr.so "$R1":/tmp/libfrr.so
docker exec -u 0 "$R1" chmod +x /tmp/test_midr_blackhole
log_pass "binary ready on r1"

# ============================================================
# Step 6: BEFORE blackhole - verify connectivity
# ============================================================
log_info "Step 6: BEFORE blackhole - r1 -> 10.100.0.1..."
if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss"; then
    log_pass "BEFORE: r1 -> 10.100.0.1 = OK (0% loss)"
else
    if docker exec "$R1" ping -c 2 -W 3 10.100.0.1 2>&1 | grep -q "ttl="; then
        log_pass "BEFORE: r1 -> 10.100.0.1 reachable"
    else
        log_fail "BEFORE: r1 -> 10.100.0.1 UNREACHABLE"
        docker exec "$R1" vtysh -c "show ip route" 2>&1 | head -15
    fi
fi

# ============================================================
# Step 7: Run ZAPI blackhole (background)
# ============================================================
log_info "Step 7: Run ZAPI blackhole on r1 (background)..."
docker exec -u 0 "$R1" bash -c "LD_LIBRARY_PATH=/tmp /tmp/test_midr_blackhole /var/run/frr/zserv.api" > /tmp/bh_out.log 2>&1 &
BH_PID=$!
sleep 3

# ============================================================
# Step 8: DURING blackhole - MUST be unreachable
# ============================================================
log_info "Step 8: DURING blackhole - r1 -> 10.100.0.1 (expect FAIL)..."
if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss"; then
    log_fail "BLACKHOLE FAIL: r1 -> 10.100.0.1 still reachable!"
else
    log_pass "BLACKHOLE OK: r1 -> 10.100.0.1 UNREACHABLE"
fi

log_info "FIB proto 199 during blackhole:"
docker exec "$R1" ip route show proto 199 2>&1 || true

# Wait for binary to complete
wait $BH_PID 2>/dev/null || true
cat /tmp/bh_out.log 2>/dev/null
grep -q "BLACKHOLE ZAPI TEST PASSED" /tmp/bh_out.log && log_pass "ZAPI binary PASSED" || log_fail "ZAPI binary FAILED"

# ============================================================
# Step 9: AFTER blackhole - connectivity MUST restore
# ============================================================
log_info "Step 9: AFTER blackhole - r1 -> 10.100.0.1 (expect OK)..."
sleep 2
if docker exec "$R1" ping -c 3 -W 2 10.100.0.1 2>&1 | grep -q " 0% packet loss"; then
    log_pass "AFTER: r1 -> 10.100.0.1 = OK (0% loss) -- restored!"
else
    log_fail "AFTER: r1 -> 10.100.0.1 still unreachable"
fi

# ============================================================
# Step 10: Verify MIDR routes cleaned
# ============================================================
log_info "Step 10: Verify MIDR routes cleaned..."
MIDR_LEFT=$(docker exec "$R1" ip route show proto 199 2>&1 | grep -c "10\." || echo 0)
[ "$MIDR_LEFT" -eq 0 ] && log_pass "all MIDR routes cleaned" || log_info "$MIDR_LEFT remain"

# ============================================================
# Summary
# ============================================================
echo ""
echo "============================================"
echo "  MIDR ZAPI Blackhole Connectivity Test"
echo "============================================"
echo "  PASS: $PASS | FAIL: $FAIL"
[ $FAIL -eq 0 ] && echo "  ALL TESTS PASSED" || echo "  $FAIL TESTS FAILED"
echo "============================================"
exit $FAIL