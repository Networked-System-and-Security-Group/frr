#!/bin/bash
# ============================================================================
# MIDR ZAPI Batch Stress Test (zebra-only, no bgpd)
#
# Tests batch route install/delete via MIDR ZAPI against a running zebra.
# Only zebra is running, routes are validated via kernel FIB (ip route show).
# ============================================================================
set -euo pipefail

PASS=0; FAIL=0
log_pass() { echo -e "\033[32m[PASS]\033[0m $*"; PASS=$((PASS + 1)); }
log_fail() { echo -e "\033[31m[FAIL]\033[0m $*"; FAIL=$((FAIL + 1)); }
log_info() { echo -e "\033[34m[INFO]\033[0m $*"; }

# ============================================================
# Step 0: Compile binary in dev container
# ============================================================
log_info "Step 0: Compile test_midr_zapi_batch..."

if [ ! -f /tmp/test_midr_zapi_batch ] || [ ! -f /tmp/libfrr.so ]; then
    sudo docker exec -u 0 frr-ubuntu24-ymy bash -c "
        cd /home/frr/frr && make bgpd/bgpd -j\$(nproc) 2>&1 | tail -2
        rm -f /tmp/test_midr_zapi_batch
        gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
          -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
          -o /tmp/test_midr_zapi_batch tests/bgpd/test_midr_zapi_batch.c \
          bgpd/bgp_midr_zebra.o \
          -L lib/.libs -lfrr -lcap -lcrypt -ljson-c -lrt -lpthread \
          -lsqlite3 -lresolv -ldl -lm -lfl -lyang 2>&1
    " || { log_fail "compile failed"; exit 1; }
    sudo docker cp frr-ubuntu24-ymy:/tmp/test_midr_zapi_batch /tmp/
    sudo docker cp frr-ubuntu24-ymy:/home/frr/frr/lib/.libs/libfrr.so.0.0.0 /tmp/libfrr.so 2>/dev/null || true
fi

[ -f /tmp/test_midr_zapi_batch ] || { log_fail "binary missing"; exit 1; }
[ -f /tmp/libfrr.so ] || { log_fail "libfrr.so missing"; exit 1; }
log_pass "binary compiled"

# ============================================================
# Step 1: Copy binary + lib into dev container
# ============================================================
log_info "Step 1: Copy binary into dev container..."
sudo docker cp /tmp/test_midr_zapi_batch frr-ubuntu24-ymy:/tmp/
sudo docker cp /tmp/libfrr.so frr-ubuntu24-ymy:/tmp/
sudo docker exec -u 0 frr-ubuntu24-ymy chmod +x /tmp/test_midr_zapi_batch
log_pass "binary ready in container"

# ============================================================
# Step 2: Start zebra in dev container (if not running)
# ============================================================
log_info "Step 2: Ensure zebra is running..."
sudo docker exec -u 0 frr-ubuntu24-ymy bash -c "
    pkill -9 zebra 2>/dev/null || true
    sleep 1
    echo -e 'bgpd=no\nzebra=yes\nstaticd=no\nospfd=no\nisisd=no\nripd=no' > /etc/frr/daemons
    /usr/lib/frr/zebra -d -u frr -g frr 2>&1 || true
    sleep 3
    pgrep zebra && echo 'zebra PID:' \$(pgrep zebra) || echo 'zebra not running'
" 2>&1

# Wait for socket
for i in $(seq 1 15); do
    if sudo docker exec frr-ubuntu24-ymy test -S /var/run/frr/zserv.api 2>/dev/null; then
        log_pass "zebra ready (${i}s)"; break
    fi
    sleep 1
done
sudo docker exec frr-ubuntu24-ymy test -S /var/run/frr/zserv.api || { log_fail "zebra socket not ready"; exit 1; }

# ============================================================
# Step 3: Run batch stress test
# ============================================================
log_info "Step 3: Run batch stress test..."
sudo docker exec -u 0 frr-ubuntu24-ymy bash -c "
    LD_LIBRARY_PATH=/tmp /tmp/test_midr_zapi_batch /var/run/frr/zserv.api
" 2>&1 | tee /tmp/batch_out.log

grep -q "ALL TESTS PASSED" /tmp/batch_out.log && log_pass "batch stress PASSED" || log_fail "batch stress FAILED"

# ============================================================
# Step 4: Verify no leftover routes
# ============================================================
log_info "Step 4: Verify cleanup..."
REMAIN=$(sudo docker exec frr-ubuntu24-ymy ip route show proto 199 2>&1 | wc -l)
[ "$REMAIN" -eq 0 ] && log_pass "all MIDR routes cleaned" || log_info "$REMAIN remain"

# ============================================================
# Summary
# ============================================================
echo ""
echo "============================================"
echo "  MIDR ZAPI Batch Stress Test"
echo "============================================"
echo "  PASS: $PASS | FAIL: $FAIL"
[ $FAIL -eq 0 ] && echo "  ALL TESTS PASSED" || echo "  $FAIL TEST(S) FAILED"
echo "============================================"
exit $FAIL