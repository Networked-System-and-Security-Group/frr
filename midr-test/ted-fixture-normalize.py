#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate a MIDR path fixture and emit normalized JSON."""

import argparse
import ipaddress
import json
import sys
from pathlib import Path

import yaml


UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT32_MAX = (1 << 31) - 1


class FixtureError(ValueError):
    pass


class UniqueKeyLoader(yaml.SafeLoader):
    pass


def construct_unique_mapping(loader, node, deep=False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise FixtureError(f"duplicate YAML key: {key}")
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_unique_mapping
)


def require_mapping(value, where):
    if not isinstance(value, dict):
        raise FixtureError(f"{where} must be a mapping")
    return value


def require_list(value, where):
    if not isinstance(value, list):
        raise FixtureError(f"{where} must be a list")
    return value


def check_keys(value, required, optional, where):
    require_mapping(value, where)
    keys = set(value)
    unknown = keys - required - optional
    missing = required - keys
    if unknown:
        raise FixtureError(
            f"{where} contains unknown key(s): {', '.join(sorted(unknown))}"
        )
    if missing:
        raise FixtureError(
            f"{where} is missing key(s): {', '.join(sorted(missing))}"
        )


def parse_int(value, minimum, maximum, where):
    if type(value) is not int or not minimum <= value <= maximum:
        raise FixtureError(f"{where} must be an integer in {minimum}..{maximum}")
    return value


def parse_router_id(value, where):
    if not isinstance(value, str):
        raise FixtureError(f"{where} must be an IPv4 Router-ID string")
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise FixtureError(f"{where} is not a valid IPv4 Router-ID") from error
    if int(address) == 0:
        raise FixtureError(f"{where} must be non-zero")
    return str(address)


def parse_address(value, where):
    if not isinstance(value, str):
        raise FixtureError(f"{where} must be an IP address string")
    try:
        return ipaddress.ip_address(value)
    except ValueError as error:
        raise FixtureError(f"{where} is not a valid IP address") from error


def parse_prefix(value, where):
    if not isinstance(value, str):
        raise FixtureError(f"{where} must be an IP prefix string")
    try:
        return str(ipaddress.ip_network(value, strict=False))
    except ValueError as error:
        raise FixtureError(f"{where} is not a valid IPv4/IPv6 prefix") from error


def normalize_nodes(values):
    nodes = []
    seen = set()
    for index, raw in enumerate(require_list(values, "nodes")):
        where = f"nodes[{index}]"
        check_keys(
            raw,
            {"node_id", "group_id"},
            {"cap_flags", "policy_tags"},
            where,
        )
        node_id = parse_router_id(raw["node_id"], f"{where}.node_id")
        if node_id in seen:
            raise FixtureError(f"{where} duplicates node_id {node_id}")
        seen.add(node_id)
        nodes.append(
            {
                "node_id": node_id,
                "group_id": parse_int(
                    raw["group_id"], 1, UINT32_MAX, f"{where}.group_id"
                ),
                "cap_flags": parse_int(
                    raw.get("cap_flags", 0),
                    0,
                    UINT64_MAX,
                    f"{where}.cap_flags",
                ),
                "policy_tags": parse_int(
                    raw.get("policy_tags", 0),
                    0,
                    UINT64_MAX,
                    f"{where}.policy_tags",
                ),
            }
        )
    if not nodes:
        raise FixtureError("nodes must not be empty")
    return nodes


def normalize_links(values, local_node_id, node_ids):
    links = []
    seen = set()
    for index, raw in enumerate(require_list(values, "links")):
        where = f"links[{index}]"
        check_keys(
            raw,
            {
                "local",
                "remote",
                "link_id",
                "local_address",
                "remote_address",
                "cost",
                "available_bandwidth_kbps",
            },
            {"local_ifindex", "policy_tags"},
            where,
        )
        local = parse_router_id(raw["local"], f"{where}.local")
        remote = parse_router_id(raw["remote"], f"{where}.remote")
        if local not in node_ids or remote not in node_ids:
            raise FixtureError(f"{where} references an unknown node")
        link_id = parse_int(raw["link_id"], 0, UINT64_MAX, f"{where}.link_id")
        identity = (local, remote, link_id)
        if identity in seen:
            raise FixtureError(f"{where} duplicates Link identity {identity}")
        seen.add(identity)

        local_address = parse_address(
            raw["local_address"], f"{where}.local_address"
        )
        remote_address = parse_address(
            raw["remote_address"], f"{where}.remote_address"
        )
        if local_address.version != remote_address.version:
            raise FixtureError(f"{where} endpoint address families differ")
        local_ifindex = parse_int(
            raw.get("local_ifindex", 0),
            0,
            INT32_MAX,
            f"{where}.local_ifindex",
        )
        if local != local_node_id and local_ifindex:
            raise FixtureError(
                f"{where}.local_ifindex is only valid for the local node"
            )

        links.append(
            {
                "local": local,
                "remote": remote,
                "link_id": link_id,
                "local_address": str(local_address),
                "remote_address": str(remote_address),
                "cost": parse_int(
                    raw["cost"], 1, UINT32_MAX - 1, f"{where}.cost"
                ),
                "available_bandwidth_kbps": parse_int(
                    raw["available_bandwidth_kbps"],
                    1,
                    UINT32_MAX,
                    f"{where}.available_bandwidth_kbps",
                ),
                "local_ifindex": local_ifindex,
                "policy_tags": parse_int(
                    raw.get("policy_tags", 0),
                    0,
                    UINT64_MAX,
                    f"{where}.policy_tags",
                ),
            }
        )
    return links


def normalize_node_prefixes(values, node_ids):
    attachments = []
    seen = set()
    for index, raw in enumerate(require_list(values, "node_prefixes")):
        where = f"node_prefixes[{index}]"
        check_keys(raw, {"prefix", "node_id"}, set(), where)
        prefix = parse_prefix(raw["prefix"], f"{where}.prefix")
        node_id = parse_router_id(raw["node_id"], f"{where}.node_id")
        if node_id not in node_ids:
            raise FixtureError(f"{where} references an unknown node")
        identity = (prefix, node_id)
        if identity in seen:
            raise FixtureError(f"{where} duplicates node-prefix attachment")
        seen.add(identity)
        attachments.append({"prefix": prefix, "node_id": node_id})
    return attachments


def normalize_prefix_groups(values, group_ids):
    mappings = []
    seen = set()
    for index, raw in enumerate(require_list(values, "prefix_groups")):
        where = f"prefix_groups[{index}]"
        check_keys(raw, {"prefix", "group_id"}, set(), where)
        prefix = parse_prefix(raw["prefix"], f"{where}.prefix")
        group_id = parse_int(
            raw["group_id"], 1, UINT32_MAX, f"{where}.group_id"
        )
        if group_id not in group_ids:
            raise FixtureError(f"{where} references an unknown group")
        identity = (prefix, group_id)
        if identity in seen:
            raise FixtureError(f"{where} duplicates prefix-group mapping")
        seen.add(identity)
        mappings.append({"prefix": prefix, "group_id": group_id})
    return mappings


def normalize_fixture(raw):
    check_keys(
        raw,
        {
            "schema_version",
            "local_node_id",
            "local_group_id",
            "nodes",
            "links",
            "node_prefixes",
            "prefix_groups",
        },
        set(),
        "root",
    )
    schema_version = parse_int(
        raw["schema_version"], 1, 1, "schema_version"
    )
    local_node_id = parse_router_id(raw["local_node_id"], "local_node_id")
    local_group_id = parse_int(
        raw["local_group_id"], 1, UINT32_MAX, "local_group_id"
    )
    nodes = normalize_nodes(raw["nodes"])
    node_by_id = {node["node_id"]: node for node in nodes}
    local = node_by_id.get(local_node_id)
    if local is None or local["group_id"] != local_group_id:
        raise FixtureError(
            "local node membership does not match local_group_id"
        )
    node_ids = set(node_by_id)
    group_ids = {node["group_id"] for node in nodes}

    return {
        "schema_version": schema_version,
        "local_node_id": local_node_id,
        "local_group_id": local_group_id,
        "nodes": nodes,
        "links": normalize_links(raw["links"], local_node_id, node_ids),
        "node_prefixes": normalize_node_prefixes(
            raw["node_prefixes"], node_ids
        ),
        "prefix_groups": normalize_prefix_groups(
            raw["prefix_groups"], group_ids
        ),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()

    try:
        with args.fixture.open("r", encoding="utf-8") as stream:
            raw = yaml.load(stream, Loader=UniqueKeyLoader)
        normalized = normalize_fixture(require_mapping(raw, "root"))
    except (OSError, yaml.YAMLError, FixtureError) as error:
        print(f"fixture error: {error}", file=sys.stderr)
        return 1

    text = json.dumps(normalized, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
