#!/bin/bash
# Verify kernel recognises protocol 199 = midr
# This proves: the MIDR DP → netlink → kernel FIB pipeline endpoint works

echo "=== Kernel Proto 199 (midr) Verification ==="

# Create dummy interface
ip link add d_midr type dummy 2>/dev/null || true
ip link set d_midr up
ip addr add 203.0.113.1/32 dev d_midr 2>/dev/null || true
sleep 0.5

# Clean stale route
ip route del 10.200.200.0/24 2>/dev/null || true

# --- Step 1: Add route with proto 199 ---
echo "[Step 1] ip route add 10.200.200.0/24 proto 199"
ip route add 10.200.200.0/24 via 203.0.113.1 dev d_midr proto 199
sleep 0.5

# --- Step 2: Show proto midr (symbolic name) ---
echo "[Step 2] ip route show proto midr"
if ip route show proto midr | grep -q "10.200.200.0/24"; then
    echo "  OK: 10.200.200.0/24 visible as proto midr"
    FOUND_NAME=1
else
    echo "  FAIL: 10.200.200.0/24 NOT in proto midr output"
    FOUND_NAME=0
fi

# --- Step 3: Show proto 199 (numeric) ---
echo "[Step 3] ip route show proto 199"
if ip route show proto 199 | grep -q "10.200.200.0/24"; then
    echo "  OK: 10.200.200.0/24 visible as proto 199"
    FOUND_NUM=1
else
    echo "  FAIL: 10.200.200.0/24 NOT in proto 199 output"
    FOUND_NUM=0
fi

# --- Step 4: Delete proto 199 ---
echo "[Step 4] ip route del 10.200.200.0/24 proto 199"
ip route del 10.200.200.0/24 proto 199
sleep 0.3

# --- Step 5: Verify deletion ---
echo "[Step 5] Verify route deleted"
if ip route show 10.200.200.0/24 2>/dev/null | grep -q "."; then
    echo "  FAIL: route still present"
    FOUND_DEL=0
else
    echo "  OK: route deleted"
    FOUND_DEL=1
fi

# Cleanup
ip link del d_midr 2>/dev/null || true

echo ""
[ "$FOUND_NAME" = 1 ] && [ "$FOUND_NUM" = 1 ] && [ "$FOUND_DEL" = 1 ] && \
    echo "=== ALL CHECKS PASSED (kernel maps proto 199 → midr) ===" && exit 0

echo "=== SOME CHECKS FAILED ===" && exit 1