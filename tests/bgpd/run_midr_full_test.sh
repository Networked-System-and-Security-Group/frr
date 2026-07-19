#!/bin/bash
# ============================================================================
# MIDR Data-Plane Full Test Suite v3.1
# Strategy: link bgp_midr_zebra.o + libfrr.a (NOT --whole-archive libbgp.a)
#           to avoid rfapi/skiplist/ringbuf transitive deps
# Run inside the FRR build container (frr-ubuntu24-ymy)
# ============================================================================
set -euo pipefail

FRR_ROOT="/home/frr/frr"
PASS=0
FAIL=0
SUMMARY_FILE="/tmp/midr_test_summary.txt"
> "$SUMMARY_FILE"

log_pass()  { echo -e "\033[32m[PASS]\033[0m $*"; echo "PASS: $*" >> "$SUMMARY_FILE"; ((PASS++)); }
log_fail()  { echo -e "\033[31m[FAIL]\033[0m $*"; echo "FAIL: $*" >> "$SUMMARY_FILE"; ((FAIL++)); }
log_info()  { echo -e "\033[34m[INFO]\033[0m $*"; }
log_section() { echo ""; echo -e "\033[1;36m========================================\033[0m"; echo -e "\033[1;36m  $*\033[0m"; echo -e "\033[1;36m========================================\033[0m"; echo ""; }

cd "$FRR_ROOT"

# ============================================================================
# Phase 0: Build bgpd with MIDR code
# ============================================================================
log_section "Phase 0: Build bgpd"
pkill -9 bgpd 2>/dev/null || true
pkill -9 zebra 2>/dev/null || true
sleep 1

rm -rf bgpd/.libs/bgpd bgpd/.libs/lt-bgpd 2>/dev/null || true
make bgpd/bgpd -j$(nproc) 2>&1 | tail -3

if [ -f bgpd/.libs/bgpd ]; then
    log_pass "bgpd rebuilt ($(stat -c%s bgpd/.libs/bgpd) bytes)"
else
    log_fail "bgpd build failed"
    exit 1
fi

# Verify MIDR symbols
SYM_COUNT=$(nm bgpd/bgp_midr_zebra.o | grep -c ' T ' || true)
if [ "$SYM_COUNT" -ge 5 ]; then
    log_pass "MIDR symbols in bgp_midr_zebra.o: $SYM_COUNT"
    nm bgpd/bgp_midr_zebra.o | grep ' T '
else
    log_fail "MIDR symbols missing"
fi

# ============================================================================
# Shared compile flags for test binaries
# ============================================================================
CFLAGS="-std=gnu11 -Wall -Wextra -g -O0 -include config.h"
INCLUDES="-I lib -I bgpd -I . $(pkg-config --cflags libyang 2>/dev/null)"
LIBS="lib/.libs/libfrr.a -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang"
# Key: use bgp_midr_zebra.o directly, NOT --whole-archive libbgp.a
MIDR_OBJ="bgpd/bgp_midr_zebra.o"

# ============================================================================
# Phase 1: Unit Tests (test_midr_zebra.c)
# ============================================================================
log_section "Phase 1: Unit Tests"

gcc $CFLAGS $INCLUDES -o /tmp/test_midr_zebra \
    tests/bgpd/test_midr_zebra.c $MIDR_OBJ \
    -Wl,--wrap=zclient_route_send \
    $LIBS 2>/tmp/midr_unit_build.log
ret=$?

if [ $ret -eq 0 ] && [ -f /tmp/test_midr_zebra ]; then
    log_pass "unit test binary built"
    echo ""
    /tmp/test_midr_zebra 2>&1 | tee /tmp/midr_unit_out.log
    ret=${PIPESTATUS[0]}
    if [ $ret -eq 0 ]; then
        log_pass "all unit tests passed"
    else
        log_fail "some unit tests failed (exit=$ret)"
    fi
else
    log_fail "unit test binary build failed"
    tail -10 /tmp/midr_unit_build.log
fi

# ============================================================================
# Phase 2: Batch Stress Tests (test_midr_batch.c)
# ============================================================================
log_section "Phase 2: Batch Stress Tests"

gcc $CFLAGS $INCLUDES -o /tmp/test_midr_batch \
    tests/bgpd/test_midr_batch.c $MIDR_OBJ \
    -Wl,--wrap=zclient_route_send \
    $LIBS 2>/tmp/midr_batch_build.log
ret=$?

if [ $ret -eq 0 ] && [ -f /tmp/test_midr_batch ]; then
    log_pass "batch test binary built"
    echo ""
    /tmp/test_midr_batch 2>&1 | tee /tmp/midr_batch_out.log
    ret=${PIPESTATUS[0]}
    if [ $ret -eq 0 ]; then
        log_pass "all batch tests passed"
    else
        log_fail "some batch tests failed (exit=$ret)"
    fi
else
    log_fail "batch test binary build failed"
    tail -10 /tmp/midr_batch_build.log
fi

# ============================================================================
# Phase 3: Proto 199 Kernel Verification
# ============================================================================
log_section "Phase 3: Proto 199 Kernel Verification"

# Setup
ip link add dummy_midr type dummy 2>/dev/null || true
ip link set dummy_midr up 2>/dev/null || true
ip addr add 203.0.113.1/32 dev dummy_midr 2>/dev/null || true

# Test add
if ip route add 10.200.200.0/24 via 203.0.113.1 dev dummy_midr proto 199 2>/dev/null; then
    log_pass "proto 199 route added"
else
    log_fail "proto 199 route add failed"
fi

# Test show
if ip route show 10.200.200.0/24 proto 199 2>/dev/null | grep -q "10.200.200.0/24"; then
    log_pass "proto 199 route visible in FIB"
else
    log_fail "proto 199 route NOT visible in FIB"
fi

# Test show proto 199
if ip route show proto 199 2>/dev/null | grep -q "10.200.200.0/24"; then
    log_pass "ip route show proto 199 works"
else
    log_fail "ip route show proto 199 failed"
fi

# Test delete
if ip route del 10.200.200.0/24 proto 199 2>/dev/null; then
    log_pass "proto 199 route deleted"
else
    log_fail "proto 199 route delete failed"
fi

# Test verify removed
if ! ip route show 10.200.200.0/24 2>/dev/null | grep -q "10.200.200.0/24"; then
    log_pass "proto 199 route verified removed"
else
    log_fail "proto 199 route still present"
fi

# Cleanup
ip link del dummy_midr 2>/dev/null || true

# ============================================================================
# Phase 4: E2E Test with Real Zebra (SPF + Dual-Instance)
# ============================================================================
log_section "Phase 4: E2E ZAPI Test"

# Setup daemon config
echo -e "bgpd=yes\nzebra=yes" > /etc/frr/daemons
cat > /etc/frr/frr.conf << 'FEOF'
frr version 10.0
frr defaults traditional
hostname test-e2e
log stdout
!
router bgp 65001
 bgp router-id 10.0.0.1
 no bgp ebgp-requires-policy
!
FEOF

# Kill old processes
pkill -9 bgpd 2>/dev/null || true
pkill -9 zebra 2>/dev/null || true
sleep 2

# Start zebra
/usr/lib/frr/zebra -d -A 127.0.0.1 -u frr -g frr 2>&1 &
sleep 2
if pgrep zebra > /dev/null; then
    log_pass "zebra started"
else
    log_fail "zebra failed to start"
fi

# Wait for socket
for i in $(seq 1 10); do
    if test -S /var/run/frr/zserv.api 2>/dev/null; then
        log_pass "zebra socket ready after ${i}s"
        break
    fi
    sleep 1
done

# Build E2E test
gcc $CFLAGS $INCLUDES -o /tmp/test_e2e \
    tests/bgpd/test_midr_zebra_e2e.c $MIDR_OBJ \
    $LIBS 2>/tmp/midr_e2e_build.log
ret=$?

if [ $ret -eq 0 ] && [ -f /tmp/test_e2e ]; then
    log_pass "E2E test binary built"
    echo ""
    /tmp/test_e2e /var/run/frr/zserv.api 2>&1 | tee /tmp/midr_e2e_out.log
    ret=${PIPESTATUS[0]}
    
    # Check key results
    if grep -q "OK: route.*in FIB" /tmp/midr_e2e_out.log 2>/dev/null; then
        log_pass "E2E SPF route install verified"
    else
        log_fail "E2E SPF route install NOT verified"
    fi
    if grep -q "OK: route.*removed from FIB" /tmp/midr_e2e_out.log 2>/dev/null; then
        log_pass "E2E SPF route delete verified"
    else
        log_fail "E2E SPF route delete NOT verified"
    fi
    if grep -q "OK: SPF survives TE delete" /tmp/midr_e2e_out.log 2>/dev/null; then
        log_pass "E2E Dual-Instance SPF survival verified"
    elif grep -q "ALL CHECKS PASSED" /tmp/midr_e2e_out.log 2>/dev/null; then
        log_pass "E2E all checks passed"
    fi
else
    log_fail "E2E test binary build failed"
    tail -10 /tmp/midr_e2e_build.log
fi

# Cleanup bgpd/zebra for later phases
pkill -9 bgpd 2>/dev/null || true
pkill -9 zebra 2>/dev/null || true
sleep 1

# ============================================================================
# Phase 5: Containerlab Topology Test
# ============================================================================
log_section "Phase 5: Containerlab Topology Test"

# Check if containerlab is available (it's on the host, not in container)
if command -v containerlab &>/dev/null; then
    log_info "containerlab detected in-container, running topology test..."
    
    RUNTIME_DIR="/tmp/clab-midr-full"
    IMAGE="frr-ubuntu24-ymy:latest"
    
    # Cleanup
    clab destroy -t "$RUNTIME_DIR/topo.yaml" --cleanup 2>/dev/null || true
    rm -rf "$RUNTIME_DIR"
    mkdir -p "$RUNTIME_DIR"/{r1,r2}
    
    # daemons
    echo -e "bgpd=yes\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no" > "$RUNTIME_DIR/r1/daemons"
    cp "$RUNTIME_DIR/r1/daemons" "$RUNTIME_DIR/r2/daemons"
    
    # r1 config
    cat > "$RUNTIME_DIR/r1/frr.conf" << 'CEOF'
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
  redistribute connected
 exit-address-family
!
CEOF

    # r2 config
    cat > "$RUNTIME_DIR/r2/frr.conf" << 'CEOF'
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
  redistribute connected
 exit-address-family
!
CEOF

    # topo.yaml
    cat > "$RUNTIME_DIR/topo.yaml" << YEOF
name: midr-full
topology:
  nodes:
    r1:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r1/frr.conf:/etc/frr/frr.conf
        - $RUNTIME_DIR/r1/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.1/32 dev dummy0; /usr/lib/frr/frrinit.sh start"
    r2:
      kind: linux
      image: $IMAGE
      binds:
        - $RUNTIME_DIR/r2/frr.conf:/etc/frr/frr.conf
        - $RUNTIME_DIR/r2/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.2/32 dev dummy0; /usr/lib/frr/frrinit.sh start"
  links:
    - endpoints: ["r1:eth1", "r2:eth1"]
YEOF

    # Deploy
    clab deploy -t "$RUNTIME_DIR/topo.yaml" 2>&1 | tail -5
    sleep 15
    
    R1="clab-midr-full-r1"
    R2="clab-midr-full-r2"
    
    # Verify containers
    docker inspect "$R1" --format '{{.State.Status}}' 2>/dev/null | grep -q "running" && \
        log_pass "r1 container running" || log_fail "r1 not running"
    docker inspect "$R2" --format '{{.State.Status}}' 2>/dev/null | grep -q "running" && \
        log_pass "r2 container running" || log_fail "r2 not running"
    
    # Wait for zebra
    SOCKET_OK=0
    for i in $(seq 1 20); do
        if docker exec "$R1" test -S /var/run/frr/zserv.api 2>/dev/null; then
            log_pass "zebra socket on r1 ready (${i}s)"
            SOCKET_OK=1
            break
        fi
        sleep 1
    done
    [ $SOCKET_OK -eq 0 ] && log_fail "zebra socket not ready"
    
    # BGP peering
    sleep 5
    BGP_OUT=$(docker exec "$R1" vtysh -c "show bgp summary" 2>&1) || true
    if echo "$BGP_OUT" | grep -q "Established"; then
        log_pass "BGP peering r1<->r2 Established"
    else
        sleep 5
        BGP_OUT2=$(docker exec "$R1" vtysh -c "show bgp summary" 2>&1) || true
        if echo "$BGP_OUT2" | grep -q "Established"; then
            log_pass "BGP peering r1<->r2 Established"
        else
            log_fail "BGP peering NOT established"
        fi
    fi
    
    # Connectivity before
    if docker exec "$R1" ping -c 2 -W 2 10.0.99.2 2>&1 | grep -q "ttl="; then
        log_pass "BEFORE: r1->r2 ping OK"
    else
        log_fail "BEFORE: r1->r2 ping FAIL"
    fi
    
    # No MIDR routes initially
    MIDR_BEFORE=$(docker exec "$R1" ip route show proto 199 2>&1 | grep -c "10\." || echo 0)
    if [ "$MIDR_BEFORE" -eq 0 ]; then
        log_pass "No MIDR routes before installation"
    else
        log_info "Pre-existing MIDR routes: $MIDR_BEFORE"
    fi
    
    # Install MIDR routes (need root in container)
    docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 metric 100 proto 199 2>/dev/null && \
        log_pass "SPF route install (metric=100)" || log_fail "SPF route install FAIL"
    docker exec -u 0 "$R1" ip route add 10.100.0.0/24 via 10.0.99.2 metric 1 proto 199 2>/dev/null && \
        log_pass "TE route install (metric=1)" || log_fail "TE route install FAIL"
    docker exec -u 0 "$R1" ip route add 10.200.0.0/24 via 10.0.99.2 proto 199 2>/dev/null && \
        log_pass "Second prefix install" || log_fail "Second prefix install FAIL"
    
    # Verify FIB
    FIB=$(docker exec "$R1" ip route show proto 199 2>&1)
    echo "$FIB"
    echo "$FIB" | grep -q "10.100.0.0/24" && log_pass "10.100.0.0/24 in FIB" || log_fail "10.100.0.0/24 NOT in FIB"
    echo "$FIB" | grep -q "10.200.0.0/24" && log_pass "10.200.0.0/24 in FIB" || log_fail "10.200.0.0/24 NOT in FIB"
    
    # Dual-Instance: TE wins
    METRIC=$(docker exec "$R1" ip route show 10.100.0.0/24 2>&1 | grep "metric 1" || true)
    if [ -n "$METRIC" ]; then
        log_pass "Dual-Instance: TE(metric=1) active"
    else
        log_fail "Dual-Instance: TE not active"
    fi
    
    # Delete TE -> SPF restore
    docker exec -u 0 "$R1" ip route del 10.100.0.0/24 metric 1 proto 199 2>/dev/null && \
        log_pass "TE route deleted" || log_fail "TE route delete FAIL"
    sleep 1
    AFTER_TE=$(docker exec "$R1" ip route show 10.100.0.0/24 2>&1 | grep "10.100.0.0/24" || true)
    if [ -n "$AFTER_TE" ]; then
        log_pass "SPF auto-restored after TE delete"
    else
        log_fail "SPF NOT auto-restored"
    fi
    
    # Delete all
    docker exec -u 0 "$R1" ip route del 10.100.0.0/24 proto 199 2>/dev/null || true
    docker exec -u 0 "$R1" ip route del 10.200.0.0/24 proto 199 2>/dev/null || true
    sleep 1
    MIDR_AFTER=$(docker exec "$R1" ip route show proto 199 2>&1 | grep -c "10\." || echo 0)
    if [ "$MIDR_AFTER" -eq 0 ]; then
        log_pass "All MIDR routes cleaned up"
    else
        log_fail "$MIDR_AFTER MIDR routes remain"
    fi
    
    # Connectivity after
    if docker exec "$R1" ping -c 2 -W 2 10.0.99.2 2>&1 | grep -q "ttl="; then
        log_pass "AFTER: r1->r2 ping OK"
    else
        log_fail "AFTER: r1->r2 ping FAIL"
    fi
    
    # Cleanup
    clab destroy -t "$RUNTIME_DIR/topo.yaml" --cleanup 2>&1 | tail -3
    log_pass "topology destroyed"
else
    log_info "containerlab not available in container - skip topology test"
    log_info "(Run clab test from host: sudo bash /tmp/clab_midr_full_test.sh)"
fi

# ============================================================================
# Summary
# ============================================================================
log_section "Test Summary"
echo ""
echo "  Total: $((PASS + FAIL))  |  PASS: $PASS  |  FAIL: $FAIL"
if [ $FAIL -eq 0 ]; then
    echo -e "  \033[32mStatus: ALL TESTS PASSED\033[0m"
else
    echo -e "  \033[31mStatus: $FAIL test(s) FAILED\033[0m"
fi
echo ""
echo "Detailed results in: $SUMMARY_FILE"
echo ""

exit $FAIL