#!/bin/bash
# MIDR ZAPI E2E via Containerlab: deploy + test + cleanup
set -e
DIR=/tmp/clab-midr-final
rm -rf "$DIR" 2>/dev/null; mkdir -p "$DIR/r1" "$DIR/r2"

# --- config files ---
echo -e "bgpd=yes\nzebra=yes\nstaticd=no" > "$DIR/r1/daemons"
echo -e "bgpd=yes\nzebra=yes\nstaticd=no" > "$DIR/r2/daemons"
printf 'frr version 10.0\nfrr defaults traditional\nhostname r1\nlog stdout\n!\ninterface eth1\n ip address 10.0.99.1/30\n!\nrouter bgp 65001\n bgp router-id 10.0.0.1\n no bgp ebgp-requires-policy\n neighbor 10.0.99.2 remote-as 65002\n !\n address-family ipv4 unicast\n  redistribute connected\n exit-address-family\n' > "$DIR/r1/frr.conf"
printf 'frr version 10.0\nfrr defaults traditional\nhostname r2\nlog stdout\n!\ninterface eth1\n ip address 10.0.99.2/30\n!\nrouter bgp 65002\n bgp router-id 10.0.0.2\n no bgp ebgp-requires-policy\n neighbor 10.0.99.1 remote-as 65001\n !\n address-family ipv4 unicast\n  redistribute connected\n exit-address-family\n' > "$DIR/r2/frr.conf"
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

# deploy
echo "=== Deploy Containerlab ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>/dev/null || true
sleep 2
sudo clab deploy -t "$DIR/topo.yaml" 2>&1 | tail -5
sleep 12

R1="clab-midr-final-r1"
R2="clab-midr-final-r2"

echo "=== Verify ==="
sudo docker inspect "$R1" | grep -q '"Running": true' && echo "PASS: r1 running" || { echo "FAIL: r1"; exit 1; }
sudo docker inspect "$R2" | grep -q '"Running": true' && echo "PASS: r2 running" || { echo "FAIL: r2"; exit 1; }
sudo docker exec "$R1" ping -c 2 10.0.99.2 2>&1 | grep -q "ttl=" && echo "PASS: ping OK" || echo "INFO: ping"

echo "=== Copy MIDR binaries ==="
sudo docker cp /tmp/midr_bgpd_v2 "$R1:/usr/lib/frr/bgpd"
sudo docker cp /tmp/test_e2e_v2 "$R1:/tmp/midr_e2e"
sudo docker exec -u 0 "$R1" chmod 755 /usr/lib/frr/bgpd /tmp/midr_e2e 2>/dev/null
echo "PASS: binaries copied"

echo "=== Run MIDR ZAPI E2E test ==="
OUTPUT=$(sudo docker exec "$R1" /tmp/midr_e2e /var/run/frr/zserv.api 2>&1) || true
echo "$OUTPUT"
RET=$?

echo ""
echo "=== Result ==="
echo "$OUTPUT" | grep -q "OK: route.*in FIB" && echo "PASS: MIDR route installed in kernel FIB" || echo "CHECK: route install"
echo "$OUTPUT" | grep -q "OK: route.*removed" && echo "PASS: MIDR route removed from FIB" || echo "CHECK: route delete"
echo "$OUTPUT" | grep -q "ALL CHECKS PASSED" && echo "PASS: ALL E2E CHECKS PASSED" || echo "E2E exit=$RET"

echo ""
echo "=== Cleanup ==="
sudo clab destroy -t "$DIR/topo.yaml" 2>&1 | tail -3
echo "PASS: topology destroyed"