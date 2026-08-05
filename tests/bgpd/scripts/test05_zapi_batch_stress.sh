#!/bin/bash
# ============================================================================
# Test 05: MIDR ZAPI Batch Stress Test (zebra-only, no bgpd)
# Purpose: Batch install/delete 64 routes via MIDR ZAPI against zebra.
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

echo "============================================"
echo "  Test 05: ZAPI Batch Stress Test"
echo "============================================"

CONTAINER=${FRR_CONTAINER:-frr-ubuntu24-ymy}
FRR_DIR=${FRR_DIR:-/home/frr/frr}
TEST_BIN="/tmp/test_midr_zapi_batch"
LIBFRR_SRC="/home/frr/frr/lib/.libs/libfrr.so.0.0.0"

# Step 1: Ensure bgpd compiled
log_info "Step 1: Ensure bgpd compiled..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR
make bgpd/bgpd -j\$(nproc) 2>&1 | tail -3
" || { log_fail "bgpd compile failed"; exit 1; }
log_pass "bgpd compiled"

# Step 2: Compile batch test binary in container
log_info "Step 2: Compile batch test binary..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR
rm -f $TEST_BIN
gcc -std=gnu11 -w -g -O0 -include config.h \
  -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
  -o $TEST_BIN tests/bgpd/test_midr_zapi_batch.c \
  bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr -lcap -lcrypt -ljson-c -lrt -lpthread \
  -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
" || { log_fail "Compile failed"; exit 1; }
log_pass "Binary compiled"

# Copy libfrr to /tmp so LD_LIBRARY_PATH can find it
sudo docker exec -u 0 "$CONTAINER" cp "$LIBFRR_SRC" /tmp/libfrr.so 2>/dev/null || true

# Step 3: Start zebra
log_info "Step 3: Start zebra daemon..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
pkill -9 zebra 2>/dev/null || true
sleep 1
echo -e 'bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no' > /etc/frr/daemons
/usr/lib/frr/zebra -d -u frr -g frr --limit-fds 100000 2>&1 || true
sleep 3
pgrep zebra && echo 'zebra OK' || echo 'zebra NOT running'
" 2>&1

for i in $(seq 1 20); do
    if sudo docker exec "$CONTAINER" test -S /var/run/frr/zserv.api 2>/dev/null; then
        log_pass "zebra ready (${i}s)"
        break
    fi
    sleep 1
done
sudo docker exec "$CONTAINER" test -S /var/run/frr/zserv.api || { log_fail "zebra socket not ready"; exit 1; }

# Step 4: Run batch stress test
log_info "Step 4: Run batch stress test..."
OUTPUT=$(sudo docker exec -u 0 "$CONTAINER" bash -c "LD_LIBRARY_PATH=/tmp /tmp/test_midr_zapi_batch /var/run/frr/zserv.api" 2>&1)
echo "$OUTPUT"

# Step 5: Parse results
if echo "$OUTPUT" | grep -q "ALL TESTS PASSED"; then
    log_pass "All batch tests passed"
else
    log_fail "Some batch tests failed"
fi

if echo "$OUTPUT" | grep -q "install PASSED"; then
    log_pass "Batch install: 64 routes to FIB"
fi
if echo "$OUTPUT" | grep -q "delete PASSED"; then
    log_pass "Batch delete: all routes removed"
fi
if echo "$OUTPUT" | grep -q "dual-instance PASSED"; then
    log_pass "Dual-Instance: SPF+TE coexist"
fi
if echo "$OUTPUT" | grep -q "SPF survives TE delete"; then
    log_pass "TE delete: SPF survives"
fi

# Step 6: Cleanup
log_info "Step 5: Cleanup..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
pkill -9 zebra 2>/dev/null || true
rm -f /tmp/test_midr_zapi_batch /tmp/libfrr.so
ip link del dummy0 2>/dev/null || true
ip route flush proto 199 2>/dev/null || true
" 2>/dev/null || true
log_pass "Cleanup done"

# Summary
echo ""
echo "============================================"
echo "  Test 05 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
[ $FAIL -eq 0 ] && echo "  ZAPI BATCH STRESS TEST PASSED" || echo "  ZAPI BATCH STRESS TEST FAILED"
echo "============================================"
exit $FAIL