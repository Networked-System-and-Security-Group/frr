#!/bin/bash
# ============================================================================
# Test 03: Unit Tests (10 tests, 46 assertions)
# Purpose: Verify bgp_midr_zebra.c internal logic via mock zclient_route_send.
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

echo "============================================"
echo "  Test 03: MIDR Unit Tests (10 tests)"
echo "============================================"

CONTAINER=${FRR_CONTAINER:-frr-ubuntu24-ymy}
FRR_DIR=${FRR_DIR:-/home/frr/frr}
TEST_BIN="/tmp/test_midr_zebra_unit"

# Step 1: Ensure bgpd is compiled (need bgp_midr_zebra.o)
log_info "Step 1: Ensure bgpd compiled..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR
make bgpd/bgpd -j\$(nproc) 2>&1 | tail -3
" || { log_fail "bgpd compile failed"; exit 1; }
log_pass "bgpd compiled"

# Step 2: Compile unit test binary
log_info "Step 2: Compile unit test..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR
rm -f $TEST_BIN
gcc -std=gnu11 -w -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  \$(pkg-config --cflags libyang 2>/dev/null) \
  -o $TEST_BIN tests/bgpd/test_midr_zebra.c bgpd/bgp_midr_zebra.o \
  -Wl,--wrap=zclient_route_send \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
" || { log_fail "Compile failed"; exit 1; }
log_pass "Binary compiled"

# Step 3: Run unit test
log_info "Step 3: Run unit tests..."
OUTPUT=$(sudo docker exec -u 0 "$CONTAINER" bash -c "LD_LIBRARY_PATH=$FRR_DIR/lib/.libs $TEST_BIN" 2>&1)
echo "$OUTPUT"

# Step 4: Parse results
if echo "$OUTPUT" | grep -q "0 test(s) FAILED"; then
    log_pass "All 10 unit tests passed"
else
    log_fail "Some unit tests failed"
fi

# Count individual test OKs
TESTS=(
    "init/fini lifecycle"
    "add/flush/diff"
    "add-then-delete"
    "deferred timer coalescing"
    "SRv6 path"
    "multiple prefixes"
    "UCMP weights"
    "empty operations"
    "struct layout"
    "dual-instance SPF+TE coexist"
)

for t in "${TESTS[@]}"; do
    if echo "$OUTPUT" | grep -q "\[Test.*${t}"; then
        log_pass "Test: $t"
    fi
done

# Cleanup
log_info "Step 4: Cleanup..."
sudo docker exec -u 0 "$CONTAINER" rm -f "$TEST_BIN" 2>/dev/null || true

# Summary
echo ""
echo "============================================"
echo "  Test 03 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
[ $FAIL -eq 0 ] && echo "  UNIT TESTS PASSED (10/10)" || echo "  UNIT TESTS FAILED"
echo "============================================"
exit $FAIL