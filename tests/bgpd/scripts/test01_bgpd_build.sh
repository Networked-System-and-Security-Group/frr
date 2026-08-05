#!/bin/bash
# ============================================================================
# Test 01: bgpd Build Verification
# ============================================================================
PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

echo "============================================"
echo "  Test 01: bgpd Build Verification"
echo "============================================"

PASSWORD=${PASSWORD:-thu325325}
CONTAINER=${FRR_CONTAINER:-frr-ubuntu24-ymy}
FRR_DIR=${FRR_DIR:-/home/frr/frr}

DS="echo $PASSWORD | sudo -S docker"

log_info "Step 1: Fix file ownership..."
eval "$DS exec -u 0 $CONTAINER bash -c 'chown -R frr:frr $FRR_DIR/bgpd/ 2>/dev/null || true'" 2>/dev/null

log_info "Step 2: Compile bgpd..."
COMPILE_OUT=$(eval "$DS exec -u 0 $CONTAINER bash -c '
cd $FRR_DIR
# Remove ALL link outputs so make re-links:
#   bgpd/.libs/bgpd  = actual binary
#   bgpd/.libs/lt-bgpd = libtool wrapper (temp)
#   bgpd/bgpd        = installed libtool wrapper script (the Makefile target)
rm -rf bgpd/.libs/bgpd bgpd/.libs/lt-bgpd bgpd/bgpd 2>/dev/null || true
make bgpd/bgpd -j\$(nproc) 2>&1
'" 2>&1)
echo "$COMPILE_OUT" | tail -20

echo "$COMPILE_OUT" | grep -q "CCLD.*bgpd" && log_pass "bgpd linked successfully" || log_fail "bgpd link failed"
echo "$COMPILE_OUT" | grep -q "CC.*bgp_midr_zebra" && log_pass "bgp_midr_zebra.o compiled" || log_info "bgp_midr_zebra.o precompiled"

log_info "Step 3: Check binary..."
BIN_SIZE=$(eval "$DS exec $CONTAINER ls -la $FRR_DIR/bgpd/.libs/bgpd 2>/dev/null" | awk '{print $5}')
[ -n "$BIN_SIZE" ] && log_pass "bgpd binary exists (${BIN_SIZE} bytes)" || log_fail "bgpd binary missing"

OBJ_SIZE=$(eval "$DS exec $CONTAINER ls -la $FRR_DIR/bgpd/bgp_midr_zebra.o 2>/dev/null" | awk '{print $5}')
[ -n "$OBJ_SIZE" ] && log_pass "bgp_midr_zebra.o exists (${OBJ_SIZE} bytes)" || log_fail "bgp_midr_zebra.o missing"

log_info "Step 4: Verify MIDR symbols..."
SYMBOLS=$(eval "$DS exec $CONTAINER bash -c 'cd $FRR_DIR && nm bgpd/bgp_midr_zebra.o | grep \" T \"'" 2>&1)
echo "$SYMBOLS"
for sym in midr_zebra_init midr_zebra_fini midr_zebra_route_add midr_zebra_route_del midr_zebra_route_flush midr_zebra_route_update_deferred; do
    echo "$SYMBOLS" | grep -q "$sym" && log_pass "symbol: $sym" || log_fail "symbol: $sym NOT FOUND"
done

log_info "Step 5: Verify MIDR in libbgp.a..."
eval "$DS exec $CONTAINER bash -c 'cd $FRR_DIR && ar t bgpd/libbgp.a'" 2>/dev/null | grep -q "bgp_midr_zebra" && log_pass "bgp_midr_zebra.o in libbgp.a" || log_fail "bgp_midr_zebra.o NOT in libbgp.a"

echo ""
echo "============================================"
echo "  Test 01 Summary: PASS=$PASS FAIL=$FAIL"
echo "============================================"
