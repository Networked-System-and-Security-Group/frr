#!/bin/bash
# Run inside docker container frr-ubuntu24-ymy
# This script: generates mock symbols, starts zebra, runs topotest
set -e
cd /home/frr/frr

echo "=== Step 1: Generate mock symbols for zebra ==="
nm -u /usr/lib/frr/zebra 2>/dev/null | grep frrscript | \
  awk '{print "void "$2"(void) {}"}' > /tmp/mock_all.c
gcc -shared -fPIC -o /tmp/mock_all.so /tmp/mock_all.c
echo "mock_all.so created"

echo "=== Step 2: Stop old zebra ==="
pkill zebra 2>/dev/null || true
sleep 1

echo "=== Step 3: Create FRR config ==="
mkdir -p /etc/frr /var/run/frr
cat > /etc/frr/zebra.conf << 'CFG'
!
log stdout
!
CFG
cat > /etc/frr/vtysh.conf << 'CFG'
!
service integrated-vtysh-config
!
CFG
cat > /etc/frr/frr.conf << 'CFG'
!
frr defaults traditional
hostname r1
log stdout
!
CFG
echo "show version" > /etc/frr/support_bundle_commands.conf

echo "=== Step 4: Start zebra ==="
LD_PRELOAD=/tmp/mock_all.so /usr/lib/frr/zebra -d \
  -f /etc/frr/zebra.conf \
  -z /var/run/frr/zserv.api
sleep 2
ps aux | grep "[/]zebra" | head -2

echo "=== Step 5: Run topotest ==="
cd tests/topotests
pytest -v -s midr_dp_topo1/test_midr_dp_topo1.py
RC=$?

echo "=== Step 6: Cleanup ==="
pkill zebra 2>/dev/null || true
exit $RC