#!/bin/bash
# ============================================================================
# Test 04: E2E ZAPI End-to-End Route Test
# Purpose: Verify MIDR DP → ZAPI → zebra → kernel FIB for SPF & Dual-Instance.
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

echo "============================================"
echo "  Test 04: E2E ZAPI Route Test"
echo "============================================"

CONTAINER="frr-ubuntu24-ymy"
TEST_BIN="/tmp/test_e2e_zapi"

# Step 1: Ensure bgpd compiled
log_info "Step 1: Ensure bgpd compiled..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd /home/frr/frr
make bgpd/bgpd -j\$(nproc) 2>&1 | tail -3
" || { log_fail "bgpd compile failed"; exit 1; }
log_pass "bgpd compiled"

# Step 2: Compile E2E test binary
log_info "Step 2: Compile E2E test..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd /home/frr/frr
rm -f $TEST_BIN
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  \$(pkg-config --cflags libyang 2>/dev/null) \
  -o $TEST_BIN tests/bgpd/test_midr_zebra_e2e.c bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
" || { log_fail "Compile failed"; exit 1; }
log_pass "Binary compiled"

# Step 3: Start zebra daemon
log_info "Step 3: Start zebra daemon..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
pkill -9 zebra 2>/dev/null || true
sleep 1
echo -e 'bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no' > /etc/frr/daemons
/usr/lib/frr/zebra -d -u frr -g frr 2>&1 || true
sleep 3
" 2>&1

# Wait for zebra socket
for i in $(seq 1 20); do
    if sudo docker exec "$CONTAINER" test -S /var/run/frr/zserv.api 2>/dev/null; then
        log_pass "zebra ready (${i}s)"
        break
    fi
    sleep 1
done
sudo docker exec "$CONTAINER" test -S /var/run/frr/zserv.api || { log_fail "zebra socket not ready"; exit 1; }

# Step 4: Run E2E test
log_info "Step 4: Run E2E ZAPI test..."
OUTPUT=$(sudo docker exec -u 0 "$CONTAINER" bash -c "LD_LIBRARY_PATH=/home/frr/frr/lib/.libs $TEST_BIN /var/run/frr/zserv.api" 2>&1)
echo "$OUTPUT"

# Step 5: Parse results
if echo "$OUTPUT" | grep -q "ALL CHECKS PASSED"; then
    log_pass "All E2E checks passed"
else
    log_fail "Some E2E checks failed"
fi

if echo "$OUTPUT" | grep -q "OK: route.*in FIB"; then
    log_pass "SPF route installed in FIB"
fi
if echo "$OUTPUT" | grep -q "OK: route.*removed from FIB"; then
    log_pass "SPF route removed from FIB"
fi
if echo "$OUTPUT" | grep -q "OK: SPF route installed"; then
    log_pass "B.1 SPF re-installed"
fi
if echo "$OUTPUT" | grep -q "SRv6 route installed"; then
    log_pass "SRv6 route installed"
elif echo "$OUTPUT" | grep -q "WARN.*SRv6"; then
    log_info "SRv6: kernel may lack seg6 support (non-critical)"
fi
if echo "$OUTPUT" | grep -q "OK: SPF survives TE delete"; then
    log_pass "SPF survives TE delete"
fi

# Step 6: Cleanup
log_info "Step 5: Cleanup..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
pkill -9 zebra 2>/dev/null || true
rm -f $TEST_BIN
ip route del 10.254.1.0/24 proto 199 2>/dev/null || true
ip link del dummy0 2>/dev/null || true
" 2>/dev/null || true
log_pass "Cleanup done"

# Summary
echo ""
echo "============================================"
echo "  Test 04 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
[ $FAIL -eq 0 ] && echo "  E2E ZAPI TEST PASSED" || echo "  E2E ZAPI TEST FAILED"
echo "============================================"
exit $FAIL