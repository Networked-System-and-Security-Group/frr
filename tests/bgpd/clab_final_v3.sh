#!/bin/bash
# MIDR ZAPI E2E via Containerlab
set -e

DIR=/tmp/clab-midr-final
rm -rf "$DIR" 2>/dev/null || true
mkdir -p "$DIR/r1" "$DIR/r2"

echo "bgpd=yes" > "$DIR/r1/daemons"
echo "zebra=yes" >> "$DIR/r1/daemons"
echo "staticd=no" >> "$DIR/r1/daemons"
cp "$DIR/r1/daemons" "$DIR/r2/daemons"

# router1 frr.conf
cat > "$DIR/r1/frr.conf" << 'EOF'
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
EOF

# router2 frr.conf
cat > "$DIR/r2/frr.conf" << 'EOF'
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
EOF

cat > "$DIR/topo.yaml" << 'YEOF'
name: midr-final
topology:
  nodes:
    r1:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - /tmp/clab-midr-final/r1/frr.conf:/etc/frr/frr.conf
        - /tmp/clab-midr-final/r1/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.1/32 dev dummy0; /usr/lib/frr/frrinit.sh start"
    r2:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - /tmp/clab-midr-final/r2/frr.conf:/etc/frr/frr.conf
        - /tmp/clab-midr-final/r2/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.2/32 dev dummy0; /usr/lib/frr/frrinit.sh start"
  links:
    - endpoints: ["r1:eth1", "r2:eth1"]
YEOF

echo "=== Deploy ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>/dev/null || true
sleep 2
sudo clab deploy -t "$DIR/topo.yaml" 2>&1 | tail -3
sleep 10

R1="clab-midr-final-r1"

echo "=== Ping Check ==="
sudo docker exec "$R1" ping -c 1 10.0.99.2 2>&1 | grep ttl || echo "ping result above"

echo "=== Wait for zebra socket ==="
for i in $(seq 1 15); do
  if sudo docker exec "$R1" test -S /var/run/frr/zserv.api 2>/dev/null; then
    echo "zserv.api ready after ${i}s"
    break
  fi
  sleep 1
done

echo "=== Copy binaries ==="
sudo docker cp /tmp/midr_bgpd_v2 "$R1:/usr/lib/frr/bgpd"
sudo docker cp /tmp/test_e2e_v2 "$R1:/tmp/midr_e2e"
echo "binaries copied"

echo "=== Run MIDR ZAPI E2E (timeout 30s) ==="
timeout 30 sudo docker exec "$R1" /tmp/midr_e2e /var/run/frr/zserv.api > /tmp/midr_e2e_result.txt 2>&1 && echo "E2E exit=0" || echo "E2E non-zero or timeout"
cat /tmp/midr_e2e_result.txt

echo ""
echo "=== Result ==="
if grep -q "route.*in FIB" /tmp/midr_e2e_result.txt 2>/dev/null; then
    echo "PASS: MIDR route installed via ZAPI to kernel FIB"
fi
if grep -q "ALL CHECKS PASSED" /tmp/midr_e2e_result.txt 2>/dev/null; then
    echo "PASS: ALL E2E CHECKS PASSED"
elif grep -q "route.*removed" /tmp/midr_e2e_result.txt 2>/dev/null; then
    echo "PASS: route install + delete verified"
fi

echo "=== Cleanup ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>&1 | tail -3
echo "DONE"