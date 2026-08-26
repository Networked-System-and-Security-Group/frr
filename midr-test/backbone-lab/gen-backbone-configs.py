#!/usr/bin/env python3
"""生成 midr-backbone 新台子的 clab yaml + 14 份 frr.conf（批 4 建场地）。

编址（与 12 节点 lab 不重叠，防日志看串）：
  1xx = 专职引导 b1-b5   11x = 群1   12x = 群2   13x = 群3(预留不部署)
  19x = 零配置 z1/z2     2xx = 纯过境 t1-t3
  rid 10.0.0.<n>  lo(transport) 10.99.0.<n>  ASN 650<n>  →  实为 65<n>
"""
import os

BASE = "/home/scripts/TsingHua/containerlab"
CFG = os.path.join(BASE, "configs-backbone")
LOG = os.path.join(BASE, "logs-backbone")

# name -> (n, role, group, extra)
NODES = {
    "b1": (101, "bootstrap", None, {}),
    "b2": (102, "bootstrap", None, {}),
    "b3": (103, "bootstrap", None, {}),
    "b4": (104, "bootstrap", None, {}),
    "b5": (105, "bootstrap", None, {}),
    "t1": (201, "transit", None, {}),
    "t2": (202, "transit", None, {}),
    "t3": (203, "transit", None, {}),
    "r1": (111, "rep", 1, {}),
    "m1a": (112, "member", 1, {}),
    "m1b": (113, "member", 1, {}),
    "r2": (121, "rep", 2, {}),
    "m2a": (122, "member", 2, {}),
    "z1": (191, "zeroconf", None, {}),
    "z2": (192, "zeroconf", None, {}),
}

BOOTSTRAPS = ["b1", "b2", "b3", "b4", "b5"]

# (a, b, /30 第三段)：a 取 .1，b 取 .2
LINKS = [
    ("t1", "t2", 1), ("t2", "t3", 2), ("t1", "t3", 3),
    ("b1", "t1", 11), ("b2", "t1", 12), ("b3", "t2", 13),
    ("b4", "t3", 14), ("b5", "t3", 15),
    ("r1", "t1", 21), ("m1a", "t1", 22), ("m1b", "t2", 23),
    ("r2", "t2", 24), ("m2a", "t3", 25),
    ("z1", "t3", 31), ("z2", "t1", 32),
]


def asn(n):
    return 65000 + n


def rid(n):
    return "10.0.0.%d" % n


def lo(n):
    return "10.99.0.%d" % n


# 每节点接口分配：按 LINKS 顺序，各自 eth1,eth2,...
ifaces = {name: [] for name in NODES}   # [(ifname, myip/30, peer, peerip)]
for a, b, sub in LINKS:
    ia = "eth%d" % (len(ifaces[a]) + 1)
    ib = "eth%d" % (len(ifaces[b]) + 1)
    ipa, ipb = "10.10.%d.1" % sub, "10.10.%d.2" % sub
    ifaces[a].append((ia, ipa, b, ipb))
    ifaces[b].append((ib, ipb, a, ipa))


def gen_conf(name):
    n, role, group, _ = NODES[name]
    L = []
    A = L.append
    A("frr version 10.7.0-dev")
    A("frr defaults traditional")
    A("hostname %s" % name)
    A("log file /etc/frr/logs/frr.log")
    A("!")
    for ifn, myip, peer, _pip in ifaces[name]:
        # ⚠ FRR 不支持行内注释（`!` 只能整行开头），对端标注必须单独成行
        A("! %s -> %s" % (ifn, peer))
        A("interface %s" % ifn)
        A(" ip address %s/30" % myip)
        A("!")
    A("interface lo")
    A(" ip address %s/32" % lo(n))
    A("!")
    if role != "transit":
        A("! RFC 8212：本台子**不**用 `no bgp ebgp-requires-policy`（那开关是整实例的，")
        A("! 写了连动态 overlay 会话一起放行，8212 就永不触发）。underlay 邻居按真实网络")
        A("! 的做法显式挂 permit 策略；overlay 自动会话有意不预放行——它正是 8212 要考的。")
        A("route-map UNDERLAY-PERMIT permit 10")
        A("!")
    A("router bgp %d" % asn(n))
    A(" bgp router-id %s" % rid(n))
    if role == "transit":
        A(" no bgp ebgp-requires-policy")
    if role != "transit":
        A(" midr transport-address %s" % lo(n))
    for _ifn, _myip, peer, pip in ifaces[name]:
        A(" neighbor %s remote-as %d" % (pip, asn(NODES[peer][0])))
    A(" !")
    A(" address-family ipv4 unicast")
    A("  redistribute connected")
    if role != "transit":
        for _ifn, _myip, _peer, pip in ifaces[name]:
            A("  neighbor %s route-map UNDERLAY-PERMIT in" % pip)
            A("  neighbor %s route-map UNDERLAY-PERMIT out" % pip)
    A(" exit-address-family")
    A(" !")
    if role != "transit":
        A(" address-family link-state link-state")
        # 引导也 distribute（08-21）：引导照发自身 Node NLRI、群号恒 0，消费侧由
        # CL getter 滤群 0 兜住。原先"引导不 distribute"那套已随代码一起撤。
        A("  distribute bgp-fabric-link-state")
        A("  ! 静态邻居（过境）一律**不** activate：过境不跑 MIDR，LS 只走 overlay 多跳")
        A("  ! 会话——这就是新台子相对 12 lab 的\"真分离\"。")
        A(" exit-address-family")
        A(" !")
    if role == "bootstrap":
        A(" midr role bootstrap")
        A(" ! 引导专职：不配 midr group-id（群号恒 0）。骨干两两手工互指 + 名单互配，")
        A(" ! 两者缺一不可：session 管\"连上\"，bootstrap 管\"认识\"（子稿 §2①）。")
        for other in BOOTSTRAPS:
            if other == name:
                continue
            A(" midr session %s remote-as %d" %
              (lo(NODES[other][0]), asn(NODES[other][0])))
        for other in BOOTSTRAPS:
            if other == name:
                continue
            A(" midr bootstrap %s remote-as %d router-id %s" %
              (lo(NODES[other][0]), asn(NODES[other][0]),
               rid(NODES[other][0])))
    elif role in ("rep", "member"):
        A(" midr group-id %d" % group)
        if role == "rep":
            A(" midr role group-rep")
        for b in BOOTSTRAPS:
            A(" midr bootstrap %s remote-as %d router-id %s" %
              (lo(NODES[b][0]), asn(NODES[b][0]), rid(NODES[b][0])))
    elif role == "zeroconf":
        A(" ! 零配置实验节点（批 4 旧败④ / 批 5 场景 B）：只配 transport + 引导候选，")
        A(" ! **不配群号**、不配角色。不属常驻拓扑。")
        for b in BOOTSTRAPS:
            A(" midr bootstrap %s remote-as %d router-id %s" %
              (lo(NODES[b][0]), asn(NODES[b][0]), rid(NODES[b][0])))
    A("!")
    return "\n".join(L) + "\n"


def gen_yaml():
    L = ["name: midr-backbone", "",
         "# 13 节点真分离台子（保底轮 2 批 4）+ z1/z2 零配置实验节点（不属常驻拓扑）。",
         "# MIDR 节点之间零直连：所有 MIDR 会话强制多跳 overlay，8212 的触发环境。",
         "", "topology:", "  nodes:"]
    for name in NODES:
        L.append("    %s:" % name)
        L.append("      kind: linux")
        L.append("      image: frr-ubuntu20:latest")
        L.append("      binds:")
        L.append("        - configs-backbone/%s/frr.conf:/etc/frr/frr.conf" % name)
        L.append("        - configs-backbone/%s/daemons:/etc/frr/daemons" % name)
        L.append("        - logs-backbone/%s:/etc/frr/logs" % name)
        L.append("      exec:")
        for c in ["mkdir -p /var/lib/frr", "chown frr:frr /var/lib/frr",
                  "chown frr:frr /etc/frr/frr.conf", "chmod 640 /etc/frr/frr.conf",
                  "chmod 777 /etc/frr", "chown -R frr:frr /etc/frr/logs",
                  "/usr/lib/frr/frrinit.sh start"]:
            L.append("        - %s" % c)
        L.append("")
    L.append("  links:")
    for a, b, _s in LINKS:
        ia = [x[0] for x in ifaces[a] if x[2] == b][0]
        ib = [x[0] for x in ifaces[b] if x[2] == a][0]
        L.append("    - endpoints: [%s:%s, %s:%s]" % (a, ia, b, ib))
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    daemons = open(os.path.join(BASE, "configs-midr/g1a/daemons")).read()
    for name in NODES:
        d = os.path.join(CFG, name)
        os.makedirs(d, exist_ok=True)
        os.makedirs(os.path.join(LOG, name), exist_ok=True)
        open(os.path.join(d, "frr.conf"), "w").write(gen_conf(name))
        open(os.path.join(d, "daemons"), "w").write(daemons)
    open(os.path.join(BASE, "midr-backbone.clab.yaml"), "w").write(gen_yaml())
    print("生成完毕：%d 个节点，%d 条链路" % (len(NODES), len(LINKS)))
    for name in NODES:
        n, role, g, _ = NODES[name]
        print("  %-4s %-9s rid=%-11s lo=%-12s AS%-6d group=%s ifs=%d" %
              (name, role, rid(n), lo(n), asn(n), g if g else "-", len(ifaces[name])))
