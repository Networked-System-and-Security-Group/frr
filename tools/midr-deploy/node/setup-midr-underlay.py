#!/usr/bin/env python3
"""Keep MIDR transport traffic on the site's underlay gateway (IPv6 FITI)."""
import ipaddress
import json
import os
import subprocess

TABLE = 1990
PRIORITY = 19900
OWNER = 199
# The configured locator is the MIDR transport endpoint.  Its UDP traffic
# includes control, PM and ephemeral traceroute ports; business sources use
# the main FIB.  TCP is limited to the two MIDR service ports.
SELECTORS = (
    ('tcp', 'dport', '5859'), ('tcp', 'sport', '5859'),
    ('tcp', 'dport', '5871'), ('tcp', 'sport', '5871'),
    ('udp', None, None), ('gre', None, None),
)


def ip(*args):
    return subprocess.run(['ip', '-6', *map(str, args)], check=True,
                          text=True, capture_output=True).stdout


def main():
    locator = ipaddress.IPv6Address(os.environ['MIDR_LOCATOR'])
    gateway = ipaddress.IPv6Address(os.environ['MIDR_GATEWAY'])
    device = os.environ.get('MIDR_UNDERLAY_INTERFACE', 'ens5')
    addresses = json.loads(subprocess.check_output(
        ['ip', '-j', '-6', 'addr', 'show', 'dev', device], text=True))
    assigned = [a for iface in addresses for a in iface.get('addr_info', [])
                if a.get('local') == str(locator)]
    if len(assigned) != 1:
        raise SystemExit('underlay interface must carry MIDR_LOCATOR exactly once')
    network = ipaddress.IPv6Network(
        f'{locator}/{assigned[0]["prefixlen"]}', strict=False)
    if gateway not in network and not gateway.is_link_local:
        raise SystemExit('MIDR_GATEWAY is not on the underlay interface')
    rules = json.loads(ip('-j', 'rule', 'show'))
    owned = []
    for rule in rules:
        priority = rule.get('priority', -1)
        if PRIORITY <= priority < PRIORITY + len(SELECTORS):
            if str(rule.get('protocol')) != str(OWNER) or str(rule.get('table')) != str(TABLE):
                raise SystemExit(f'policy rule priority {priority} is already in use')
            owned.append(priority)
    routes = subprocess.run(['ip', '-j', '-6', 'route', 'show', 'table', str(TABLE)],
                            text=True, capture_output=True)
    if routes.returncode and 'FIB table does not exist' not in routes.stderr:
        raise SystemExit(routes.stderr)
    for route in json.loads(routes.stdout) if routes.returncode == 0 else []:
        if route.get('dev') != device or route.get('dst') not in (str(network), 'default'):
            raise SystemExit(f'underlay table {TABLE} is already in use')
    # Install the gateway before rules can send any traffic to this table.
    ip('route', 'replace', str(network), 'dev', device, 'src', str(locator),
       'table', TABLE, 'proto', 'static')
    ip('route', 'replace', 'default', 'via', str(gateway), 'dev', device,
       'src', str(locator), 'table', TABLE, 'proto', 'static')
    for priority in owned:
        ip('rule', 'del', 'pref', priority)
    for offset, (proto, port_kind, port) in enumerate(SELECTORS):
        args = ['rule', 'add', 'pref', PRIORITY + offset, 'from', str(locator) + '/128',
                'ipproto', proto]
        if port_kind:
            args += [port_kind, port]
        ip(*args, 'lookup', TABLE, 'protocol', OWNER)
    print(f'MIDR underlay: table={TABLE} gateway={gateway} dev={device}; '
          'control/PM/trace/GRE separated from the main business FIB')


if __name__ == '__main__':
    main()
