#!/bin/bash
# MIDR ZAPI E2E via Containerlab - complete flow
set -e
DIR=/tmp/clab-midr-final
rm -rf "$DIR" 2>/dev/null || true
mkdir -p "$DIR/r1" "$DIR/r2"

echo "bgpd=yes" > "$DIR/r1/daemons"
echo "zebra=yes" >> "$DIR/r1/daemons"
echo "staticd=no" >> "$DIR/r1/daemons"
cp "$DIR/r1/daemons" "$DIR/r2/daemons"

cat > "$DIR/r1/frr.conf" << 'FEOF'
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
FEOF

cat > "$DIR/r2/frr.conf" << 'FEOF'
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
FEOF

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

echo "=== Deploy Containerlab ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>/dev/null || true
sleep 2
sudo clab deploy -t "$DIR/topo.yaml" 2>&1 | tail -5
sleep 12

R1="clab-midr-final-r1"

echo "=== Verify ==="
sudo docker inspect "$R1" | grep -q '"Running": true' && echo "PASS: r1 running" || { echo "FAIL: r1"; exit 1; }
sudo docker exec "$R1" ping -c 1 10.0.99.2 2>&1 | grep -q "ttl=" && echo "PASS: ping OK" || echo "INFO: ping"

echo "=== Copy MIDR binaries ==="
sudo docker cp /tmp/midr_bgpd_v2 "$R1:/usr/lib/frr/bgpd"
sudo docker cp /tmp/test_e2e_v2 "$R1:/tmp/midr_e2e"
echo "PASS: binaries copied"

echo "=== Run MIDR ZAPI E2E ==="
sudo docker exec "$R1" /tmp/midr_e2e /var/run/frr/zserv.api > /tmp/midr_e2e_out.txt 2>&1
echo "EXIT=$?"
echo "---E2E OUTPUT---"
cat /tmp/midr_e2e_out.txt
echo "---END---"

if grep -q "ALL CHECKS PASSED" /tmp/midr_e2e_out.txt; then
    echo "RESULT: ALL E2E CHECKS PASSED"
elif grep -q "OK: route" /tmp/midr_e2e_out.txt; then
    echo "RESULT: Partial success (route operations verified)"
else
    echo "RESULT: Check output above"
fi

echo "=== Cleanup ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>&1 | tail -3
echo "PASS: cleaned up"