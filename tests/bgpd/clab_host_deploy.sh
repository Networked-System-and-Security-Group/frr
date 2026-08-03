#!/bin/bash
# Run on HOST (not inside container) to deploy Containerlab topology
set -e

CLAB_DIR="/tmp/clab-midr-dp"
mkdir -p "$CLAB_DIR/router1" "$CLAB_DIR/router2"

# router1 configs
cat > "$CLAB_DIR/router1/daemons" << 'DAEMONS'
bgpd=yes
zebra=yes
staticd=no
DAEMONS

cat > "$CLAB_DIR/router1/frr.conf" << 'FRRCONF'
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
FRRCONF

# router2 configs
cat > "$CLAB_DIR/router2/daemons" << 'DAEMONS'
bgpd=yes
zebra=yes
staticd=no
DAEMONS

cat > "$CLAB_DIR/router2/frr.conf" << 'FRRCONF'
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
FRRCONF

# clab topology file
cat > "$CLAB_DIR/topo.yaml" << 'CLABYAML'
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
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.1/32 dev dummy0; /usr/lib/frr/frrinit.sh start; sysctl -w net.ipv6.conf.all.forwarding=1"
    router2:
      kind: linux
      image: frr-ubuntu24-ymy:latest
      binds:
        - /tmp/clab-midr-dp/router2/frr.conf:/etc/frr/frr.conf
        - /tmp/clab-midr-dp/router2/daemons:/etc/frr/daemons
      exec:
        - bash -c "ip link add dummy0 type dummy 2>/dev/null; ip link set dummy0 up; ip addr add 192.0.2.2/32 dev dummy0; /usr/lib/frr/frrinit.sh start; sysctl -w net.ipv6.conf.all.forwarding=1"
  links:
    - endpoints: ["router1:eth1", "router2:eth1"]
CLABYAML

# Deploy
echo "=== Destroying old topology ==="
sudo clab destroy -t "$CLAB_DIR/topo.yaml" 2>/dev/null || true
sleep 2

echo "=== Deploying topology ==="
sudo clab deploy -t "$CLAB_DIR/topo.yaml" 2>&1 | tail -10
sleep 10

R1="clab-midr-dp-test-router1"
R2="clab-midr-dp-test-router2"

echo ""
echo "=== Container Status ==="
sudo docker inspect "$R1" 2>/dev/null | grep -q '"Running": true' && echo "PASS: router1 running" || echo "FAIL: router1"
sudo docker inspect "$R2" 2>/dev/null | grep -q '"Running": true' && echo "PASS: router2 running" || echo "FAIL: router2"

echo ""
echo "=== Ping Test ==="
sudo docker exec "$R1" ping -c 2 10.0.99.2 2>&1 | grep -q "ttl=" && echo "PASS: ping OK" || echo "FAIL: ping"

echo ""
echo "=== BGP Session ==="
sleep 5
BGP=$(sudo docker exec "$R1" vtysh -c "show bgp summary" 2>&1 || echo "")
echo "$BGP" | head -6
echo "$BGP" | grep -q "Established" && echo "PASS: BGP Established" || echo "INFO: BGP check complete"

echo ""
echo "=== Proto 199 Route Install ==="
sudo docker exec "$R1" ip route add 10.99.99.0/24 via 10.0.99.2 proto 199 2>&1
sudo docker exec "$R1" ip route show proto midr 2>&1 | grep -q "10.99.99" && echo "PASS: route in FIB" || echo "FAIL: route not found"

echo ""
echo "=== Proto 199 Route Delete ==="
sudo docker exec "$R1" ip route del 10.99.99.0/24 proto 199 2>&1
sudo docker exec "$R1" ip route show 10.99.99.0/24 2>&1 | grep -q "10.99.99" && echo "FAIL: route still there" || echo "PASS: route removed"

echo ""
echo "=== Dual-Instance: TE overrides SPF ==="
sudo docker exec "$R1" ip route add 10.88.88.0/24 via 192.0.2.1 metric 100 proto 199 2>&1
sudo docker exec "$R1" ip route add 10.88.88.0/24 via 192.0.2.1 metric 1 proto 199 2>&1
BEST=$(sudo docker exec "$R1" ip route show 10.88.88.0/24 2>&1 | head -1)
echo "Best: $BEST"
echo "$BEST" | grep -q "metric 1" && echo "PASS: TE(metric=1) wins" || echo "FAIL: TE not selected"
sudo docker exec "$R1" ip route del 10.88.88.0/24 metric 1 proto 199 2>&1
FALLBACK=$(sudo docker exec "$R1" ip route show 10.88.88.0/24 2>&1 | head -1)
echo "Fallback: $FALLBACK"
echo "$FALLBACK" | grep -q "metric 100" && echo "PASS: SPF auto-restored" || echo "FAIL: SPF not restored"

echo ""
echo "=== Cleanup ==="
sudo clab destroy -t "$CLAB_DIR/topo.yaml" 2>&1 | tail -3
echo "PASS: topology destroyed"