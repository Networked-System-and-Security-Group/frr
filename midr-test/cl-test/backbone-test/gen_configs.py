#!/usr/bin/env python3
"""Generate backbone-test/configs/bgpd-*.conf with real underlay eBGP.

Run from midr-test/cl-test/backbone-test/. Regenerates every file in
configs/ (including new ones for t1-t3, which never ran bgpd before).

Why this exists (2026-08-24): the group1/group2 integration moved MIDR-LS
Link NLRI generation to code that needs a real, interface-adjacent BGP peer
per link -- a session dialed purely from static routes (this topology's
original design, see setup.sh's old header note) fails to originate Link
NLRI ("missing local link-id and interface") and the session drops ~80s
later. Fixed the same way midr-test/cl-test/group-alloc-test/setup.sh was:
every node gets a real `interface ethN` + `neighbor ... remote-as ...`
underlay eBGP session per physical link, redistributing connected routes.
Kernel packet forwarding is unaffected -- bgpd runs with -Z (no zebra), so
these underlay BGP sessions never touch the kernel FIB; setup.sh's existing
static routes still do all the real forwarding, exactly as before. This is
purely to give the new Link NLRI code a real peer to originate against.

Addressing (transport 10.99.0.<n>, router-id 10.0.0.<n>, ASN 65000+n) and
the link table are copied verbatim from setup.sh so the two stay in sync --
if you change setup.sh's L1..L15 table, update LINKS below to match.
"""

import os

N = {
    "b1": 101, "b2": 102, "b3": 103, "b4": 104, "b5": 105,
    "r1": 111, "m1a": 112, "m1b": 113,
    "r2": 121, "m2a": 122,
    "z1": 191, "z2": 192,
    "t1": 201, "t2": 202, "t3": 203,
}
TRANSIT = {"t1", "t2", "t3"}
GROUP = {"r1": 1, "m1a": 1, "m1b": 1, "r2": 2, "m2a": 2}
ROLE = {**{b: "bootstrap" for b in ["b1", "b2", "b3", "b4", "b5"]},
        "r1": "rep", "r2": "rep", "m1a": "member", "m1b": "member", "m2a": "member",
        "z1": "zeroconf", "z2": "zeroconf"}
BOOTSTRAPS = ["b1", "b2", "b3", "b4", "b5"]

# (node1, node2, subnet-third-octet, delay, bandwidth) -- must match setup.sh's L1..L15
LINKS = [
    ("t1", "t2", 1, "1ms", "100mbit"),
    ("t2", "t3", 2, "1ms", "100mbit"),
    ("t1", "t3", 3, "1ms", "100mbit"),
    ("b1", "t1", 11, "1ms", "50mbit"),
    ("b2", "t1", 12, "1ms", "50mbit"),
    ("b3", "t2", 13, "1ms", "50mbit"),
    ("b4", "t3", 14, "1ms", "50mbit"),
    ("b5", "t3", 15, "1ms", "50mbit"),
    ("r1", "t1", 21, "1ms", "50mbit"),
    ("m1a", "t1", 22, "1ms", "50mbit"),
    ("m1b", "t2", 23, "2ms", "50mbit"),
    ("r2", "t2", 24, "25ms", "20mbit"),
    ("m2a", "t3", 25, "2ms", "50mbit"),
    ("z1", "t3", 31, "0ms", "100mbit"),
    ("z2", "t1", 32, "0ms", "100mbit"),
]


def asn(n): return 65000 + n
def rid(n): return "10.0.0.%d" % n
def lo(n): return "10.99.0.%d" % n


# Assign per-node eth1/eth2/... in LINKS order, same as setup.sh's next_if().
ifaces = {name: [] for name in N}  # name -> [(ifname, my_ip, peer, peer_ip, peer_asn)]
ifcount = {name: 0 for name in N}
for a, b, sub, delay, bw in LINKS:
    ifcount[a] += 1
    ifcount[b] += 1
    aeth, beth = "eth%d" % ifcount[a], "eth%d" % ifcount[b]
    aip, bip = "10.10.%d.1" % sub, "10.10.%d.2" % sub
    ifaces[a].append((aeth, aip, b, bip, asn(N[b])))
    ifaces[b].append((beth, bip, a, aip, asn(N[a])))


def gen_transit(name):
    L = []
    A = L.append
    A("! Node %s: transit fabric (no MIDR role -- underlay-only eBGP,"
      " redistribute connected). Router-ID %s | ASN %d" % (name, rid(N[name]), asn(N[name])))
    A("frr version 10.0")
    A("hostname node-bb-%s" % name)
    A("log file logs/bgpd-%s.log debugging" % name)
    A("log timestamp precision 3")
    A("")
    for ifn, myip, peer, _pip, _pasn in ifaces[name]:
        A("! %s -> %s" % (ifn, peer))
        A("interface %s" % ifn)
        A(" ip address %s/30" % myip)
        A("!")
    A("router bgp %d" % asn(N[name]))
    A("  bgp router-id %s" % rid(N[name]))
    A("  no bgp ebgp-requires-policy")
    A("  no bgp network import-check")
    A("")
    for _ifn, _myip, _peer, pip, pasn in ifaces[name]:
        A("  neighbor %s remote-as %d" % (pip, pasn))
    A("  !")
    A("  address-family ipv4 unicast")
    A("    redistribute connected")
    A("  exit-address-family")
    A("!")
    return "\n".join(L) + "\n"


def gen_real(name):
    n = N[name]
    role = ROLE[name]
    L = []
    A = L.append
    A("! Node %s: %s. Transport(lo) %s | Router-ID %s | ASN %d" %
      (name, role, lo(n), rid(n), asn(n)))
    A("frr version 10.0")
    A("hostname node-bb-%s" % name)
    A("log file logs/bgpd-%s.log debugging" % name)
    A("log timestamp precision 3")
    A("")
    A("debug bgp midr")
    if role == "zeroconf":
        A("debug bgp link-state")
    A("")
    for ifn, myip, peer, _pip, _pasn in ifaces[name]:
        A("! %s -> %s" % (ifn, peer))
        A("interface %s" % ifn)
        A(" ip address %s/30" % myip)
        A("!")
    A("interface lo")
    A(" ip address %s/32" % lo(n))
    A("!")
    A("route-map UNDERLAY-PERMIT permit 10")
    A("!")
    A("router bgp %d" % asn(n))
    A("  bgp router-id %s" % rid(n))
    A("  no bgp network import-check")
    A("")
    A("  midr transport-address %s" % lo(n))
    if role == "bootstrap":
        A("  midr role bootstrap")
        for other in BOOTSTRAPS:
            if other == name:
                continue
            A("  midr session %s remote-as %d" % (lo(N[other]), asn(N[other])))
        for other in BOOTSTRAPS:
            if other == name:
                continue
            A("  midr bootstrap %s remote-as %d router-id %s" %
              (lo(N[other]), asn(N[other]), rid(N[other])))
    elif role in ("rep", "member"):
        A("  midr group-id %d" % GROUP[name])
        if role == "rep":
            A("  midr role group-rep")
        for b in BOOTSTRAPS:
            A("  midr bootstrap %s remote-as %d router-id %s" %
              (lo(N[b]), asn(N[b]), rid(N[b])))
    elif role == "zeroconf":
        A("  ! zero-config join-flow test node: transport + bootstrap candidates only,")
        A("  ! no group-id (config_group_id 0 is now rejected outright), no role.")
        for b in BOOTSTRAPS:
            A("  midr bootstrap %s remote-as %d router-id %s" %
              (lo(N[b]), asn(N[b]), rid(N[b])))
    A("")
    for _ifn, _myip, _peer, pip, pasn in ifaces[name]:
        A("  neighbor %s remote-as %d" % (pip, pasn))
    A("  !")
    A("  address-family ipv4 unicast")
    A("    redistribute connected")
    for _ifn, _myip, _peer, pip, _pasn in ifaces[name]:
        A("    neighbor %s route-map UNDERLAY-PERMIT in" % pip)
        A("    neighbor %s route-map UNDERLAY-PERMIT out" % pip)
    A("  exit-address-family")
    A("!")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    outdir = os.path.join(os.path.dirname(__file__), "configs")
    os.makedirs(outdir, exist_ok=True)
    for name in N:
        text = gen_transit(name) if name in TRANSIT else gen_real(name)
        path = os.path.join(outdir, "bgpd-%s.conf" % name)
        with open(path, "w") as f:
            f.write(text)
        print("  wrote %s (%d interfaces)" % (path, len(ifaces[name])))
    print("Done: %d configs (%d transit, %d real)." %
          (len(N), len(TRANSIT), len(N) - len(TRANSIT)))
