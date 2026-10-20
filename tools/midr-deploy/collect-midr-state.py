#!/usr/bin/env python3
"""Collect role, protocol and kernel evidence from the MIDR nodes of a profile."""
import argparse
import concurrent.futures
import csv
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


REMOTE = r'''
python3 - <<'PY'
import json, os, pathlib, subprocess
env = dict(line.split('=', 1) for line in pathlib.Path('/etc/midr/node.env').read_text().splitlines() if '=' in line)
result = {'env': env, 'commands': pathlib.Path('/etc/midr/node.commands').read_text()}
os.environ['LD_LIBRARY_PATH'] = env['MIDR_RELEASE_DIR'] + '/lib' + (':' + os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else '')
def run(key, args):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=25)
        result[key] = {'rc': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}
    except subprocess.TimeoutExpired:
        result[key] = {'rc': 124, 'stdout': '', 'stderr': 'command timeout'}
release = env['MIDR_RELEASE_DIR']
for unit in ('midr-zebra', 'midrd', 'midr-bgpd', 'midr-bgp-prefix-provider'):
    run(unit, ['systemctl', 'is-active', unit])
for command in ('self', 'join', 'neighbors', 'nodes', 'directory', 'links', 'spf', 'virtual-links', 'running-config'):
    run(command, ['python3', release + '/midrd-vty.py', '--socket', '/run/midr/midrd.vty', '-c', 'show midr ' + command])
for family in ('4', '6'):
    run('fib' + family, ['ip', '-' + family, 'route', 'show', 'proto', '199'])
    run('routes' + family, ['ip', '-j', '-' + family, 'route', 'show'])
run('default', ['ip', '-6', 'route', 'show', 'default'])
run('rules6', ['ip', '-6', 'rule', 'show'])
run('underlay6', ['ip', '-6', 'route', 'show', 'table', '1990'])
run('processes', ['ps', '-eo', 'pid,comm,args'])
run('journal', ['journalctl', '-u', 'midrd', '-u', 'midr-bgpd', '-u', 'midr-bgp-prefix-provider', '-n', '60', '--no-pager'])
if env['MIDR_EDGE_BGP'] == 'yes':
    run('bgp', [release + '/vtysh', '--vty_socket', '/run/midr', '-d', 'bgpd', '-c', 'show bgp ipv6 unicast summary json'])
print(json.dumps(result, ensure_ascii=False))
PY
'''


def collect(node, output):
    args = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
            *shlex.split(os.environ.get('MIDR_SSH_EXTRA', '')),
            '-p', node['ssh_port'], node['ssh_user'] + '@' + node['ssh_host'], 'bash', '-s']
    try:
        p = subprocess.run(args, input=REMOTE, capture_output=True, text=True, timeout=300)
        if p.returncode:
            raise RuntimeError(p.stderr.strip() or p.stdout.strip())
        data = json.loads(p.stdout)
        (output / (node['name'] + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
        text = lambda key: data.get(key, {}).get('stdout', '')
        match = re.search(r'(?m)^Group-ID\s*:\s*(\d+)', text('self'))
        group = match.group(1) if match else '?'
        summary = re.search(r'Summary: reachable (\d+) local (\d+) intra (\d+) inter (\d+)', text('spf'))
        scope = summary.groups() if summary else ('?',) * 4
        return [node['name'], node['role'], node['edge_bgp'], group,
                *(text(unit).strip() for unit in ('midr-zebra', 'midrd', 'midr-bgpd', 'midr-bgp-prefix-provider')),
                str(len(re.findall(r'\bEstablished\b', text('neighbors')))),
                str(len(re.findall(r'\bup\b', text('links')))), *scope,
                str(len(re.findall(r'\bREADY\b', text('virtual-links')))),
                str(len(re.findall(r'READY.*overlay reachable', text('virtual-links')))),
                str(len(text('fib4').splitlines()) + len(text('fib6').splitlines())),
                'OK' if all(data[key]['rc'] == 0 for key in ('self', 'join', 'neighbors', 'links', 'spf', 'virtual-links')) else 'CLI_FAILED']
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        (output / (node['name'] + '.error.txt')).write_text(str(exc) + '\n')
        return [node['name'], node['role'], node['edge_bgp'], *(['?'] * 14), 'COLLECT_FAILED']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    profile = Path(os.environ.get('MIDR_PROFILE',
                                  Path(__file__).resolve().parent / 'profiles' / 'fiti'))
    parser.add_argument('--profile', type=Path, default=profile,
                        help='profile directory providing nodes.csv (default: $MIDR_PROFILE or profiles/fiti)')
    parser.add_argument('--nodes', type=Path, default=None,
                        help='explicit nodes.csv, overriding the profile')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4)
    args = parser.parse_args()
    nodes_file = args.nodes or args.profile / 'nodes.csv'
    with nodes_file.open() as stream:
        nodes = [row for row in csv.DictReader(stream)
                 if row.get('name') and not row['name'].lstrip().startswith('#')]
    raw = args.output / 'raw'
    raw.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        rows = list(pool.map(lambda node: collect(node, raw), nodes))
    with (args.output / 'summary.tsv').open('w') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
        writer.writerow(['node', 'role', 'edge_bgp', 'group', 'zebra', 'midrd', 'bgpd', 'provider',
                         'sessions', 'up_links', 'reachable', 'local', 'intra', 'inter',
                         'gre_ready', 'gre_reachable', 'proto199', 'collection'])
        writer.writerows(rows)
    print((args.output / 'summary.tsv').read_text(), end='')
    if any(row[-1] != 'OK' for row in rows):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
