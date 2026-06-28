#!/bin/bash
# Run on server (101.6.30.220) to sync files and execute docker test
# Usage: ./run_topomidr_on_server.sh
set -e

PASS=thu325325
CONTAINER=frr-ubuntu24-ymy
SRC=/home/yangmy/frr
FRR_CONTAINER_PATH=/home/frr/frr

echo "=== Step 1: Copy topotest folder into container ==="
echo "$PASS" | sudo -S docker cp "$SRC/tests/topotests/midr_dp_topo1" "$CONTAINER:$FRR_CONTAINER_PATH/tests/topotests/"

echo "=== Step 2: Copy all source files into container ==="
for f in \
  bgpd/bgp_midr_zebra.c bgpd/bgp_midr_zebra.h bgpd/bgpd.h bgpd/subdir.am \
  lib/frrdistance.h lib/route_types.txt \
  tools/etc/iproute2/rt_protos.d/frr.conf \
  zebra/debug_nl.c zebra/fpm_listener.c zebra/kernel_netlink.c \
  zebra/rt_netlink.c zebra/rt_netlink.h zebra/zebra_rib.c \
  tests/bgpd/test_midr_zebra.c tests/bgpd/test_midr_zebra_e2e.c; do
  echo "$PASS" | sudo -S docker cp "$SRC/$f" "$CONTAINER:$FRR_CONTAINER_PATH/$f"
done

echo "=== Step 3: Copy docker script into container ==="
echo "$PASS" | sudo -S docker cp /home/yangmy/frr/scripts/docker_midr_test.sh "$CONTAINER:/tmp/docker_midr_test.sh"
echo "$PASS" | sudo -S docker exec -u 0 "$CONTAINER" chmod +x /tmp/docker_midr_test.sh

echo "=== Step 4: Run docker script ==="
echo "$PASS" | sudo -S docker exec -u 0 "$CONTAINER" bash /tmp/docker_midr_test.sh

echo "=== DONE ==="