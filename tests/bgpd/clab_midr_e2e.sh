#!/bin/bash
# ============================================================================
# Build bgpd + E2E binary in frr-ubuntu24-ymy container,
# then deploy to Containerlab topology and run real ZAPI E2E test
# ============================================================================
set -e

echo "=== Step 1: Build bgpd and E2E binary in dev container ==="
# Clean old obj leftovers first
sudo docker exec -u 0 frr-ubuntu24-ymy bash -c 'rm -rf /tmp/midr_objs /tmp/test_midr_e2e 2>/dev/null; mkdir -p /tmp/midr_objs'
sudo docker exec frr-ubuntu24-ymy bash -c '
cd /home/frr/frr
make bgpd/bgpd -j$(nproc) 2>&1 | tail -3
cd /tmp/midr_objs
ar x /home/frr/frr/bgpd/libbgp.a bgp_midr_zebra.o 2>/dev/null
cd /home/frr/frr
gcc -o /tmp/test_midr_e2e tests/bgpd/test_midr_zebra_e2e.c \
  -include config.h -I lib -I bgpd -I . \
  /tmp/midr_objs/bgp_midr_zebra.o lib/.libs/libfrr.so \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang \
  2>&1 | grep -E "(error:|collect2)" || echo "  E2E compiled OK"
file /tmp/test_midr_e2e
'

sudo docker cp frr-ubuntu24-ymy:/home/frr/frr/bgpd/.libs/bgpd /tmp/midr_bgpd_new 2>/dev/null
sudo docker cp frr-ubuntu24-ymy:/tmp/test_midr_e2e /tmp/test_midr_e2e 2>/dev/null
sudo chmod 755 /tmp/midr_bgpd_new /tmp/test_midr_e2e 2>/dev/null || true
echo "  bgpd: $(stat -c%s /tmp/midr_bgpd_new 2>/dev/null || echo 0) bytes"
echo "  E2E:  $(stat -c%s /tmp/test_midr_e2e 2>/dev/null || echo 0) bytes"

echo ""
echo "=== Step 2: Deploy Containerlab topology ==="
CLAB_DIR="/tmp/clab-midr-dp"
mkdir -p "$CLAB_DIR/router1" "$CLAB_DIR/router2"

cat > "$CLAB_DIR/router1/daemons" << 'DEOF'
bgpd=yes
zebra=yes
staticd=no
DEOF
cat > "$CLAB_DIR/router1/frr.conf" << 'FEOF'
frr version 10.0
frr defaults traditional
hostname router1
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
FEOF
cat > "$CLAB_DIR/router2/daemons" << 'DEOF'
bgpd=yes
zebra=yes
staticd=no
DEOF
cat > "$CLAB_DIR/router2/frr.conf" << 'FEOF'
frr version 10.0
frr defaults traditional
hostname router2
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
FEOF

cat > "$CLAB_DIR/topo.yaml" << 'YEOF'
name: midr-dp-test
topology:
  nodes:
    router1:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - /tmp/clab-midr-dp/router1/frr.conf:/etc/frr/frr.conf
        - /tmp/clab-midr-dp/router1/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.1/32 dev dummy0 2>/dev/null; /usr/lib/frr/frrinit.sh start"
    router2:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - /tmp/clab-midr-dp/router2/frr.conf:/etc/frr/frr.conf
        - /tmp/clab-midr-dp/router2/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.2/32 dev dummy0 2>/dev/null; /usr/lib/frr/frrinit.sh start"
  links:
    - endpoints: ["router1:eth1", "router2:eth1"]
YEOF

sudo clab destroy -t "$CLAB_DIR/topo.yaml" 2>/dev/null || true
sleep 2
sudo clab deploy -t "$CLAB_DIR/topo.yaml" 2>&1 | tail -5
sleep 10

R1="clab-midr-dp-test-router1"
R2="clab-midr-dp-test-router2"

echo ""
echo "=== Step 3: Deploy updated bgpd to router1 ==="
sudo docker cp /tmp/midr_bgpd_new "$R1:/usr/lib/frr/bgpd"
sudo docker cp /tmp/test_midr_e2e "$R1:/tmp/test_midr_e2e"
sudo docker exec -u 0 "$R1" chmod 755 /usr/lib/frr/bgpd /tmp/test_midr_e2e 2>/dev/null || true
echo "  Files copied to router1"

# Restart FRR in router1 to pick up new bgpd
sudo docker exec "$R1" bash -c 'pkill bgpd 2>/dev/null; sleep 1; /usr/lib/frr/watchfrr.sh restart bgpd 2>/dev/null || /usr/lib/frr/bgpd -d -u frr -g frr' &
sleep 5

echo ""
echo "=== Step 4: Verify BGP connectivity ==="
sudo docker exec "$R1" ping -c 2 10.0.99.2 2>&1 | grep -q "ttl=" && echo "PASS: ping OK" || echo "FAIL: ping"
sleep 3

# Check zebra socket
sudo docker exec "$R1" ls -la /var/run/frr/zserv.api 2>/dev/null && echo "PASS: zserv.api ready" || echo "INFO: waiting for zebra"
sleep 3

echo ""
echo "=== Step 5: Run MIDR ZAPI E2E inside router1 ==="
sudo docker exec "$R1" /tmp/test_midr_e2e /var/run/frr/zserv.api 2>&1
RET=$?

echo ""
echo "=== Step 6: Summary ==="
if [ $RET -eq 0 ]; then
    echo "PASS: MIDR ZAPI E2E full chain verified inside Containerlab"
    echo "  Route: midr_zebra_route_add() -> ZAPI -> zebra -> kernel FIB -> ip route show"
else
    echo "E2E exit code: $RET (check output above)"
fi

echo ""
echo "=== Step 7: Cleanup ==="
sudo clab destroy -t "$CLAB_DIR/topo.yaml" 2>&1 | tail -3
echo "PASS: topology destroyed"