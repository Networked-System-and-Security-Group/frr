#!/bin/bash
# ============================================================================
# Test 02: Netlink C Protocol 199 Verification
# Purpose: Verify Linux kernel accepts RTPROT_BGP_MIDR=199 via libnl-3/netlink.
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

echo "============================================"
echo "  Test 02: Netlink C Proto 199 Verification"
echo "============================================"

CONTAINER=${FRR_CONTAINER:-frr-ubuntu24-ymy}
FRR_DIR=${FRR_DIR:-/home/frr/frr}
TEST_BIN="/tmp/test_midr_proto199"

# Step 1: Compile the test binary inside the container
log_info "Step 1: Compile test_midr_proto199..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
cd $FRR_DIR/tests/bgpd
rm -f $TEST_BIN
gcc -std=gnu11 -Wall -Wextra -g -O0 \
    -I/usr/include/libnl3 \
    test_midr_proto199.c \
    -lnl-3 -lnl-route-3 -lm \
    -o $TEST_BIN 2>&1
" || { log_fail "Compile failed"; exit 1; }
log_pass "Binary compiled"

# Step 2: Ensure /etc/iproute2/rt_protos has proto 199 mapping
log_info "Step 2: Ensure proto 199 mapping..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
grep -q '199.*midr' /etc/iproute2/rt_protos 2>/dev/null || echo '199  midr' >> /etc/iproute2/rt_protos
" || true
log_pass "proto 199 mapping ensured"

# Step 3: Run the test binary
log_info "Step 3: Run test_midr_proto199..."
OUTPUT=$(sudo docker exec -u 0 "$CONTAINER" "$TEST_BIN" 2>&1)
echo "$OUTPUT"

# Step 4: Parse results
if echo "$OUTPUT" | grep -q "ALL CHECKS PASSED"; then
    log_pass "All netlink checks passed"
else
    log_fail "Netlink checks failed"
fi

if echo "$OUTPUT" | grep -q "Route added via netlink"; then
    log_pass "Route added via netlink"
fi
if echo "$OUTPUT" | grep -q "shows as proto midr"; then
    log_pass "Route visible as proto midr"
fi
if echo "$OUTPUT" | grep -q "shows as proto 199"; then
    log_pass "Route visible as proto 199"
fi
if echo "$OUTPUT" | grep -q "route deleted"; then
    log_pass "Route deleted successfully"
fi

# Step 5: Cleanup residual routes
log_info "Step 4: Cleanup..."
sudo docker exec -u 0 "$CONTAINER" bash -c "
ip route del 10.200.200.0/24 proto 199 2>/dev/null || true
ip link del dummy_nl 2>/dev/null || true
" 2>/dev/null || true
log_pass "Cleanup done"

# Summary
echo ""
echo "============================================"
echo "  Test 02 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
[ $FAIL -eq 0 ] && echo "  PROTO 199 NETLINK C VERIFICATION PASSED" || echo "  PROTO 199 NETLINK C VERIFICATION FAILED"
echo "============================================"
exit $FAIL