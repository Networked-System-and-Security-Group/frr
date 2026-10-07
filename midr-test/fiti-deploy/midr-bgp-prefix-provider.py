#!/usr/bin/env python3
"""Publish the local BGP RIB to the standalone MIDR Prefix IPC socket.

The provider deliberately talks to bgpd through its public JSON CLI.  It does
not link against FRR internals and does not make midrd depend on bgpd.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import logging
import os
import socket
import struct
import subprocess
import time
from dataclasses import dataclass
from typing import Any, Iterable


MAGIC = 0x4D504658
VERSION = 1
SNAPSHOT_BEGIN = 1
UPSERT = 2
WITHDRAW = 3
SNAPSHOT_END = 4
EOR = 5
FRAME_LEN = 48


@dataclass(frozen=True)
class Prefix:
    network: ipaddress._BaseNetwork
    metric: int = 0

    @property
    def family(self) -> int:
        return 4 if self.network.version == 4 else 6

    def frame_address(self) -> bytes:
        raw = self.network.network_address.packed
        return raw + bytes(16 - len(raw))


def truthy(value: Any) -> bool:
    return value is True or value == 1 or value == "true"


def route_prefix(value: str) -> ipaddress._BaseNetwork | None:
    try:
        if "/" not in value:
            return None
        return ipaddress.ip_network(value, strict=False)
    except ValueError:
        return None


def iter_route_entries(value: Any) -> Iterable[tuple[str, dict[str, Any]]]:
    """Find FRR route dictionaries without depending on one JSON wrapper."""
    if isinstance(value, dict):
        for key, child in value.items():
            if isinstance(key, str) and route_prefix(key):
                # FRR has emitted both {prefix: {paths: [...]}} and
                # {prefix: [{...}, {...}]} shapes over its JSON history.
                if isinstance(child, dict):
                    if any(k in child for k in ("paths", "path", "bestPath", "bestpath", "valid")):
                        yield key, child
                    continue
                elif isinstance(child, list):
                    route_children = [
                        item for item in child
                        if isinstance(item, dict)
                        and (
                            "paths" in item
                            or isinstance(item.get("path"), (dict, list))
                        )
                    ]
                    if route_children:
                        for route in route_children:
                            yield key, route
                    else:
                        yield key, {"paths": child}
                    continue
            yield from iter_route_entries(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_route_entries(child)


def path_is_usable(path: dict[str, Any]) -> bool:
    for key in ("stale", "withdrawn", "removed", "inactive", "invalid"):
        if truthy(path.get(key)):
            return False
    if "valid" in path and not truthy(path["valid"]):
        return False
    return True


def path_is_selected(path: dict[str, Any]) -> bool:
    return any(truthy(path.get(key)) for key in (
        "best", "selected", "multipath", "bestpath", "bestPath",
    ))


def path_metric(path: dict[str, Any], route: dict[str, Any]) -> int:
    value = path.get("metric", route.get("metric", 0))
    try:
        return max(0, min(int(value), 0xFFFFFFFF))
    except (TypeError, ValueError):
        return 0


def route_paths(route: dict[str, Any]) -> list[dict[str, Any]]:
    if "paths" in route:
        paths = route["paths"]
    elif isinstance(route.get("path"), (dict, list)):
        paths = route["path"]
    elif any(key in route for key in (
        "valid", "best", "bestpath", "bestPath", "selected", "multipath",
    )):
        # Some FRR versions put a single selected path directly under the
        # prefix key and use a string-valued AS path in ``path``.
        paths = [route]
    else:
        paths = []
    if isinstance(paths, dict):
        paths = [paths]
    return [path for path in paths if isinstance(path, dict)] if isinstance(paths, list) else []


def parse_bgp_json(payload: Any, excluded: set[ipaddress._BaseNetwork]) -> dict[str, Prefix]:
    result: dict[str, Prefix] = {}
    seen: set[tuple[str, str]] = set()
    for text, route in iter_route_entries(payload):
        network = route_prefix(text)
        if network is None or network in excluded:
            continue
        key = (str(network), text)
        if key in seen:
            continue
        seen.add(key)
        paths = route_paths(route)
        usable = [path for path in paths if path_is_usable(path)]
        if not usable:
            continue
        selected = [path for path in usable if path_is_selected(path)]
        if not selected:
            # Some FRR versions omit best/selected for a route with one valid
            # path.  Accept that unambiguous case only.
            if len(usable) != 1:
                continue
            selected = usable
        metric = min(path_metric(path, route) for path in selected)
        result[str(network)] = Prefix(network, metric)
    return result


def frame(kind: int, generation: int, originator: int, prefix: Prefix | None = None) -> bytes:
    data = bytearray(FRAME_LEN)
    struct.pack_into("!I", data, 0, MAGIC)
    data[4] = VERSION
    data[5] = kind
    struct.pack_into("!Q", data, 8, generation)
    struct.pack_into("!I", data, 16, originator)
    if prefix is not None:
        data[20] = prefix.family
        data[21] = prefix.network.prefixlen
        data[24:40] = prefix.frame_address()
        struct.pack_into("!I", data, 40, prefix.metric)
    return bytes(data)


class Provider:
    def __init__(self, args: argparse.Namespace, excluded: set[ipaddress._BaseNetwork]):
        self.args = args
        self.excluded = excluded
        self.generation = 0
        self.sock: socket.socket | None = None
        self.current: dict[str, Prefix] = {}

    def next_generation(self) -> int:
        self.generation = 1 if self.generation == 0xFFFFFFFFFFFFFFFF else self.generation + 1
        if self.generation == 0:
            self.generation = 1
        return self.generation

    def query(self, command: str) -> Any:
        completed = subprocess.run(
            [self.args.vtysh, "--vty_socket", self.args.vty_socket,
             "-d", "bgpd", "-c", command],
            check=True, capture_output=True, text=True, timeout=self.args.command_timeout,
        )
        return json.loads(completed.stdout)

    def query_prefixes(self) -> dict[str, Prefix]:
        result: dict[str, Prefix] = {}
        for command in ("show bgp ipv4 unicast json", "show bgp ipv6 unicast json"):
            payload = self.query(command)
            result.update(parse_bgp_json(payload, self.excluded))
        return result

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def connect(self) -> None:
        self.close()
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.args.socket_timeout)
        sock.connect(self.args.prefix_socket)
        self.sock = sock
        logging.info("connected to %s", self.args.prefix_socket)

    def send(self, kind: int, prefix: Prefix | None = None) -> None:
        if self.sock is None:
            raise ConnectionError("Prefix IPC is not connected")
        generation = self.generation
        self.sock.sendall(frame(kind, generation, self.args.originator, prefix))

    def send_snapshot(self, prefixes: dict[str, Prefix]) -> None:
        generation = self.next_generation()
        logging.info("publishing snapshot generation=%d prefixes=%d", generation, len(prefixes))
        self.send(SNAPSHOT_BEGIN)
        for key in sorted(prefixes):
            self.send(UPSERT, prefixes[key])
        self.send(SNAPSHOT_END)
        self.send(EOR)
        self.current = dict(prefixes)

    def publish_changes(self, prefixes: dict[str, Prefix]) -> None:
        removed = sorted(set(self.current) - set(prefixes))
        changed = sorted(key for key in set(prefixes) & set(self.current)
                         if prefixes[key] != self.current[key])
        added = sorted(set(prefixes) - set(self.current))
        for key in removed:
            self.next_generation()
            self.send(WITHDRAW, self.current[key])
            logging.info("withdraw %s generation=%d", key, self.generation)
        for key in changed + added:
            self.next_generation()
            self.send(UPSERT, prefixes[key])
            logging.info("upsert %s generation=%d", key, self.generation)
        self.current = dict(prefixes)

    def run(self) -> None:
        while True:
            try:
                prefixes = self.query_prefixes()
                if self.sock is None:
                    self.connect()
                    self.send_snapshot(prefixes)
                elif prefixes != self.current:
                    self.publish_changes(prefixes)
            except (OSError, ConnectionError, subprocess.SubprocessError, ValueError, json.JSONDecodeError) as exc:
                logging.warning("provider cycle failed: %s", exc)
                self.close()
            time.sleep(self.args.interval)


def read_excludes(args: argparse.Namespace) -> set[ipaddress._BaseNetwork]:
    values = list(args.exclude_prefix)
    if args.exclude_prefix_file:
        with open(args.exclude_prefix_file, encoding="utf-8") as stream:
            values.extend(line.split("#", 1)[0].strip() for line in stream)
    excluded: set[ipaddress._BaseNetwork] = set()
    for value in values:
        if not value:
            continue
        network = route_prefix(value)
        if network is None:
            raise ValueError(f"invalid excluded prefix: {value}")
        excluded.add(network)
    return excluded


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtysh", required=True)
    parser.add_argument("--vty-socket", default="/run/midr")
    parser.add_argument("--prefix-socket", default="/run/midr/prefix.sock")
    parser.add_argument("--originator", required=True, type=int)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--command-timeout", type=float, default=10.0)
    parser.add_argument("--socket-timeout", type=float, default=10.0)
    parser.add_argument("--exclude-prefix", action="append", default=[])
    parser.add_argument("--exclude-prefix-file")
    parser.add_argument("--log-level", default="INFO",
                        choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    args = parser.parse_args()
    if not os.path.isfile(args.vtysh) or not os.access(args.vtysh, os.X_OK):
        parser.error(f"vtysh is not executable: {args.vtysh}")
    if args.originator <= 0 or args.originator > 0xFFFFFFFF:
        parser.error("originator must be in 1..4294967295")
    if args.interval <= 0:
        parser.error("interval must be positive")
    return args


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level),
                        format="%(asctime)s %(levelname)s %(message)s")
    try:
        excluded = read_excludes(args)
        Provider(args, excluded).run()
    except (OSError, ValueError) as exc:
        logging.error("provider stopped: %s", exc)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
