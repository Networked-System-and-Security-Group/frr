#!/usr/bin/env bash
# Start one lab node inside its container: node.sh NAME
set -euo pipefail
n=$1
export LD_LIBRARY_PATH=/opt/midr/lib:/opt/hostlib
R=/run/midr
ip -6 route del default dev eth0 2>/dev/null || true
mkdir -p $R /var/log/midr /etc/midr /var/lib/midr /usr/local/libexec/midr

case $n in
gwa|gwb)
    s=${n#gw}
    ip -6 addr add 2001:db8:$s:1::1/64 dev eth1 nodad
    ip -6 addr add 2001:db8:ff::$([ "$s" = a ] && echo 1 || echo 2)/64 dev eth2 nodad
    ip -6 addr add 2001:db8:$s:10::1/128 dev lo
    ip -6 route add blackhole 2001:db8:$s::/48
    ip link set eth1 up; ip link set eth2 up
    sysctl -qw net.ipv6.conf.all.forwarding=1
    /opt/midr/zebra -u root -g root -f /lab/cfg/$n-zebra.conf -i $R/zebra.pid \
        -z $R/zserv.api --vty_socket $R -d --log file:/var/log/midr/zebra.log
    sleep 1
    /opt/midr/bgpd -u root -g root -f /lab/cfg/$n-bgpd.conf -i $R/bgpd.pid \
        -z $R/zserv.api --vty_socket $R -d -p 179 --log file:/var/log/midr/bgpd.log
    ;;
ma|mb)
    # Same placement as install-node-local.sh.
    cp /tools/node/*.sh /tools/node/setup-midr-underlay.py /usr/local/libexec/midr/
    cp /opt/midr/midr-bgp-prefix-provider.py /usr/local/libexec/midr/
    cp /lab/cfg/$n-node.env /etc/midr/node.env
    cp /lab/cfg/$n-node.commands /etc/midr/node.commands
    cp /lab/cfg/$n-bgpd.conf /etc/midr/bgpd.conf
    cp /lab/locators.txt /etc/midr/locators.txt
    set -a; . /etc/midr/node.env; set +a
    bash /usr/local/libexec/midr/midr-export-config.sh /etc/midr/bgpd.conf \
        /etc/midr/locators.txt "$MIDR_LOCATOR" 199 >>/etc/midr/bgpd.conf
    printf 'hostname %s-zebra\n' $n >/etc/midr/zebra.conf
    ip -6 addr add "$MIDR_LOCATOR/64" dev eth1 nodad
    ip link set eth1 up
    sysctl -qw net.ipv6.conf.all.forwarding=1
    ip -6 route replace default via "$MIDR_GATEWAY" dev eth1
    L=/usr/local/libexec/midr
    setsid $L/start-midr-zebra.sh </dev/null >/dev/null 2>&1 &
    for i in $(seq 50); do [ -S $R/zserv.api ] && break; sleep 0.2; done
    setsid $L/start-midr-bgpd.sh </dev/null >/dev/null 2>&1 &
    python3 $L/setup-midr-underlay.py
    setsid $L/start-midrd-service.sh </dev/null >/dev/null 2>&1 &
    $L/load-midrd-config.sh >/var/log/midr/load.log 2>&1
    setsid $L/start-midr-bgp-prefix-provider.sh </dev/null >/dev/null 2>&1 &
    ;;
esac
echo "$n started"
