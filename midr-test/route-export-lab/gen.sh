#!/usr/bin/env bash
# Two-site lab for MIDR reverse export, shaped like a FITI edge site.
#
#   ta(lo) gwa ===underlay eBGP=== gwb tb(lo)
#           |                       |
#           ma ~~~~ MIDR GRE ~~~~~ mb
#
# gwa/gwb: site gateways (BGP-R), AS 65001/65002.  Each originates its site
#          /48 and owns a loopback inside it standing in for a terminal.
# ma/mb:   MIDR edge nodes 101/102, iBGP to their gateway (same AS) with
#          next-hop-self on the gateway, as on FITI.
set -euo pipefail
SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
LAB=${MIDR_LAB_WORK:-/tmp/midr-route-export-lab}
RELEASE=${RELEASE:?set RELEASE to a midr-* release directory}
TOOLS=${TOOLS:-$(CDPATH= cd -- "$SRC/../../tools/midr-deploy" && pwd -P)}
IMAGE=frr-midr-p6:f9beedd0d653
mkdir -p "$LAB/cfg" "$LAB/profile"
cp "$SRC/node.sh" "$LAB/node.sh"

cat >"$LAB/profile/nodes.csv" <<'EOF'
name,ssh_host,ssh_port,ssh_user,node_id,locator,gateway,edge_bgp,role,group
ma,127.0.0.1,22,root,101,2001:db8:a:1::2,2001:db8:a:1::1,yes,member,1
mb,127.0.0.1,22,root,102,2001:db8:b:1::2,2001:db8:b:1::1,yes,member,1
EOF

site() {  # site letter, AS, peer gateway address, peer AS
    local s=$1 as=$2 peer=$3 peer_as=$4
    printf 'hostname gw%s\n' "$s" >"$LAB/cfg/gw$s-zebra.conf"
    # The gateway gives its MIDR node the site prefix and every locator /64
    # it learns, so the MIDR side has to filter the locators itself.
    cat >"$LAB/cfg/gw$s-bgpd.conf" <<EOF
hostname gw$s
ipv6 prefix-list TO-MIDR seq 5 permit 2001:db8:$s::/48
ipv6 prefix-list TO-MIDR seq 10 permit 2001:db8::/32 ge 64 le 64
route-map TO-MIDR permit 10
 match ipv6 address prefix-list TO-MIDR
exit
router bgp $as
 bgp router-id 10.0.0.$([ "$s" = a ] && echo 1 || echo 2)
 no bgp ebgp-requires-policy
 no bgp default ipv4-unicast
 neighbor $peer remote-as $peer_as
 neighbor 2001:db8:$s:1::2 remote-as $as
 address-family ipv6 unicast
  network 2001:db8:$s::/48
  neighbor $peer activate
  neighbor 2001:db8:$s:1::2 activate
  neighbor 2001:db8:$s:1::2 next-hop-self
  neighbor 2001:db8:$s:1::2 route-map TO-MIDR out
 exit-address-family
exit
EOF
}
site a 65001 2001:db8:ff::2 65002
site b 65002 2001:db8:ff::1 65001

midr() {  # name, id, site letter, AS, peer locator, peer MIDR id
    local n=$1 id=$2 s=$3 as=$4 peer=$5 peer_id=$6
    cat >"$LAB/cfg/$n-node.env" <<EOF
MIDR_RELEASE_DIR=/opt/midr
MIDR_NODE_NAME=$n
MIDR_NODE_ID=$id
MIDR_LOCATOR=2001:db8:$s:1::2
MIDR_GATEWAY=2001:db8:$s:1::1
MIDR_UNDERLAY_INTERFACE=eth1
MIDR_EDGE_BGP=yes
MIDR_ROLE=member
MIDR_GROUP=1
MIDR_PREFIX_MODE=socket
MIDR_PREFIX_INTERVAL=2
MIDR_ROUTE_EXPORT=yes
EOF
    cat >"$LAB/cfg/$n-node.commands" <<EOF
configure terminal
midr transport-address 2001:db8:$s:1::2
midr group-id 1
midr session $peer remote-as $((65000 + peer_id))
end
EOF
    cat >"$LAB/cfg/$n-bgpd.conf" <<EOF
hostname $n
router bgp $as
 bgp router-id $id.0.0.0
 no bgp ebgp-requires-policy
 no bgp default ipv4-unicast
 neighbor 2001:db8:$s:1::1 remote-as $as
 address-family ipv6 unicast
  neighbor 2001:db8:$s:1::1 activate
 exit-address-family
exit
EOF
}
midr ma 101 a 65001 2001:db8:b:1::2 102
midr mb 102 b 65002 2001:db8:a:1::2 101

node() {
    cat <<EOF
    $1:
      kind: linux
      image: $IMAGE
      user: root
      cmd: sleep infinity
      binds:
        - $RELEASE:/opt/midr:ro
        - /usr/lib/x86_64-linux-gnu/libunwind.so.8:/opt/hostlib/libunwind.so.8:ro
        - $TOOLS:/tools:ro
        - $LAB:/lab:ro
EOF
}
{
    echo 'name: midr-loop'
    echo 'topology:'
    echo '  nodes:'
    for n in gwa gwb ma mb; do node $n; done
    cat <<'EOF'
  links:
    - endpoints: ["ma:eth1", "gwa:eth1"]
    - endpoints: ["mb:eth1", "gwb:eth1"]
    - endpoints: ["gwa:eth2", "gwb:eth2"]
EOF
} >"$LAB/midr-loop.clab.yaml"
echo generated
