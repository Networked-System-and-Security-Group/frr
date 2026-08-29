#!/usr/bin/env python3
"""Verify captured MIDR group1-to-group2 state without changing the lab."""

from __future__ import annotations

import csv
import hashlib
import re
import sys
from pathlib import Path


MEMBERS = ("r1", "r2", "m1a", "m1b", "m2a", "z1", "z2")
EXPECTED_GROUP_SIZE = {
    "r1": 4,
    "m1a": 4,
    "m1b": 4,
    "z1": 1,
    "r2": 2,
    "m2a": 2,
    "z2": 4,
}


class Checks:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, condition: bool, message: str) -> None:
        if condition:
            self.passed += 1
            print(f"PASS: {message}")
        else:
            self.failed += 1
            print(f"FAIL: {message}")


def read_output(root: Path, node: str, slug: str) -> str:
    path = root / "nodes" / node / f"{slug}.txt"
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def text_field(text: str, label: str) -> str | None:
    match = re.search(rf"^\s*{re.escape(label)}\s*:\s*(\S+)", text, re.MULTILINE)
    return match.group(1) if match else None


def int_field(text: str, label: str) -> int | None:
    value = text_field(text, label)
    if value is None:
        return None
    match = re.match(r"(\d+)", value)
    return int(match.group(1)) if match else None


def ratio_field(text: str, label: str) -> tuple[int, int] | None:
    match = re.search(
        rf"^\s*{re.escape(label)}\s*:\s*(\d+)\s*/\s*(\d+)",
        text,
        re.MULTILINE,
    )
    return (int(match.group(1)), int(match.group(2))) if match else None


def parse_kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z][A-Za-z0-9-]*)=([^\s]+)", line))


def digest(rows: set[tuple[str, ...]]) -> str:
    payload = "\n".join("\t".join(row) for row in sorted(rows))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def parse_lsdb_objects(text: str) -> list[dict[str, str]]:
    return [
        parse_kv(line)
        for line in text.splitlines()
        if line.startswith("object type=")
    ]


def lsdb_memberships(objects: list[dict[str, str]]) -> set[tuple[str, ...]]:
    return {
        (obj["origin"], obj["group"], obj["transport"], obj["caps"])
        for obj in objects
        if obj.get("type") == "MEMBERSHIP"
        and obj.get("usable") == "yes"
        and all(key in obj for key in ("origin", "group", "transport", "caps"))
    }


def lsdb_local_memberships(
    objects: list[dict[str, str]], group_id: int
) -> set[tuple[str, ...]]:
    return {
        (obj["origin"], obj["group"], obj["caps"])
        for obj in objects
        if obj.get("type") == "MEMBERSHIP"
        and obj.get("usable") == "yes"
        and obj.get("group") == str(group_id)
        and all(key in obj for key in ("origin", "group", "caps"))
    }


def lsdb_links(
    objects: list[dict[str, str]], origin_nodes: set[str] | None = None
) -> set[tuple[str, ...]]:
    return {
        (
            obj["origin"],
            obj["remote"],
            obj["link-id"],
            obj["addresses"],
            obj["derived-cost"],
        )
        for obj in objects
        if obj.get("type") == "LINK"
        and obj.get("usable") == "yes"
        and (origin_nodes is None or obj.get("origin") in origin_nodes)
        and all(
            key in obj
            for key in ("origin", "remote", "link-id", "addresses", "derived-cost")
        )
    }


def lsdb_node_prefixes(objects: list[dict[str, str]]) -> set[tuple[str, ...]]:
    return {
        (obj["origin"], obj["afi"], obj["safi"], obj["prefix"])
        for obj in objects
        if obj.get("type") == "NODE_PREFIX"
        and obj.get("usable") == "yes"
        and all(key in obj for key in ("origin", "afi", "safi", "prefix"))
    }


def lsdb_group_prefixes(objects: list[dict[str, str]]) -> set[tuple[str, ...]]:
    return {
        (obj["group"], obj["afi"], obj["safi"], obj["prefix"])
        for obj in objects
        if obj.get("type") == "GROUP_PREFIX"
        and obj.get("usable") == "yes"
        and all(key in obj for key in ("group", "afi", "safi", "prefix"))
    }


def ted_nodes(text: str) -> set[tuple[str, ...]]:
    rows: set[tuple[str, ...]] = set()
    pattern = re.compile(r"^\s*node=(\S+) group=(\d+) caps=(\S+)")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            rows.add(match.groups())
    return rows


def ted_links(text: str) -> tuple[set[tuple[str, ...]], list[tuple[int, int, int]]]:
    rows: set[tuple[str, ...]] = set()
    egress: list[tuple[int, int, int]] = []
    in_egress = False
    pattern = re.compile(
        r"^\s*(\S+)\(group=(\d+)\)->(\S+)\(group=(\d+)\) "
        r"link-id=(\S+) addresses=(\S+) cost=(\d+)"
    )
    for line in text.splitlines():
        if line.startswith("egress-links ("):
            in_egress = True
            continue
        if line.startswith("node-prefixes ("):
            in_egress = False
        match = pattern.match(line)
        if not match:
            continue
        source, source_group, target, target_group, link_id, addresses, cost = (
            match.groups()
        )
        rows.add((source, target, link_id, addresses, cost))
        if in_egress:
            egress.append((int(source_group), int(target_group), int(cost)))
    return rows, egress


def ted_node_prefixes(text: str) -> set[tuple[str, ...]]:
    rows: set[tuple[str, ...]] = set()
    pattern = re.compile(r"^\s*node=(\S+) afi=(\d+) safi=(\d+) prefix=(\S+)")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            rows.add(match.groups())
    return rows


def ted_prefix_groups(text: str) -> set[tuple[str, ...]]:
    rows: set[tuple[str, ...]] = set()
    pattern = re.compile(r"^\s*group=(\d+) afi=(\d+) safi=(\d+) prefix=(\S+)")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            rows.add(match.groups())
    return rows


def ted_group_edges(text: str) -> set[tuple[int, int, int]]:
    rows: set[tuple[int, int, int]] = set()
    pattern = re.compile(r"^\s*group=(\d+)->(\d+) aggregate-cost=(\d+)")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            rows.add(tuple(map(int, match.groups())))
    return rows


def expected_group_edges(
    objects: list[dict[str, str]], memberships: set[tuple[str, ...]]
) -> set[tuple[int, int, int]]:
    groups = {row[0]: int(row[1]) for row in memberships if len(row) >= 2}
    costs: dict[tuple[int, int], int] = {}
    for obj in objects:
        if obj.get("type") != "LINK" or obj.get("usable") != "yes":
            continue
        source_group = groups.get(obj.get("origin", ""))
        target_group = groups.get(obj.get("remote", ""))
        cost = obj.get("derived-cost")
        if source_group is None or target_group is None or source_group == target_group:
            continue
        if cost is None:
            continue
        key = (source_group, target_group)
        cost = int(cost)
        costs[key] = min(costs.get(key, cost), cost)
    return {(source, target, cost) for (source, target), cost in costs.items()}


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} EVIDENCE_PHASE_DIR", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    checks = Checks()
    summaries: list[dict[str, int | str]] = []
    semantic_rows: list[dict[str, int | str]] = []
    all_memberships: dict[str, set[tuple[str, ...]]] = {}
    all_prefix_groups: dict[str, set[tuple[str, ...]]] = {}

    for node in MEMBERS:
        self_text = read_output(root, node, "midr-self")
        interface = read_output(root, node, "group2-interface")
        provider = read_output(root, node, "group2-provider-snapshot")
        remote_view = read_output(root, node, "group2-remote-view")
        input_sync = read_output(root, node, "input-sync")
        node_facts = read_output(root, node, "local-node-facts")
        link_facts = read_output(root, node, "local-link-facts")
        events = read_output(root, node, "input-events")
        owned = read_output(root, node, "owned-objects")
        rib_summary = read_output(root, node, "rib-summary")
        rib_paths = read_output(root, node, "rib-paths")
        lsdb_summary = read_output(root, node, "lsdb-summary")
        lsdb_text = read_output(root, node, "lsdb-objects")
        ted_summary = read_output(root, node, "ted-summary")
        ted_text = read_output(root, node, "ted-detail")

        router_id = text_field(self_text, "Router-ID")
        group_id = int_field(self_text, "Group-ID")
        expected_size = EXPECTED_GROUP_SIZE[node]

        node_report = re.search(
            r"node\s*: valid=(\d+) reported=(\d+) pending=(\d+) version=(\d+)",
            interface,
        )
        link_report = re.search(
            r"link\s*:.*?(\d+).*?reported\s+(\d+)\s*/\s*pending\s+(\d+)",
            interface,
        )
        reported_node = int(node_report.group(2)) if node_report else -1
        pending_node = int(node_report.group(3)) if node_report else -1
        fact_link_entries = int(link_report.group(1)) if link_report else -1
        reported_links = int(link_report.group(2)) if link_report else -1
        pending_links = int(link_report.group(3)) if link_report else -1

        snapshot_status = re.search(r"snapshot_get\(\)\s*=\s*(-?\d+)", provider)
        snapshot_nodes = int_field(provider, "node_count")
        snapshot_links = int_field(provider, "link_count")
        active_node_lines = [
            line
            for line in node_facts.splitlines()
            if re.match(r"^\d+\.\d+\.\d+\.\d+ group \d+ .* active$", line)
        ]
        active_link_lines = [
            line
            for line in link_facts.splitlines()
            if re.match(r"^\d+\.\d+\.\d+\.\d+ -> .* active$", line)
        ]

        owned_memberships = int_field(owned, "memberships")
        owned_links = int_field(owned, "links")
        owned_sequence_failures = int_field(owned, "sequence failures")
        owned_fightbacks = int_field(owned, "fightbacks")

        identities_ratio = ratio_field(rib_summary, "identities")
        identities = identities_ratio[0] if identities_ratio else -1
        rib_path_count = int_field(rib_summary, "paths")
        selected = int_field(rib_summary, "selected")
        conflicts = int_field(rib_summary, "conflicts")
        payload_conflicts = int_field(rib_summary, "payload conflicts")
        selected_lines = [
            line
            for line in rib_paths.splitlines()
            if line.lstrip().startswith("path ") and "selected=yes" in line
        ]
        remote_selected = [line for line in selected_lines if "peer=self" not in line]
        multihop_selected = [
            line
            for line in remote_selected
            if re.search(r"propagation=\[[^]]*,[^]]*\]", line)
        ]

        lsdb_objects_count = int_field(lsdb_summary, "objects")
        lsdb_usable = int_field(lsdb_summary, "usable")
        lsdb_pending = int_field(lsdb_summary, "pending")
        lsdb_membership_count = int_field(lsdb_summary, "memberships")
        lsdb_dirty = int_field(lsdb_summary, "dirty entries")
        lsdb_commits = ratio_field(lsdb_summary, "commits/failures")
        objects = parse_lsdb_objects(lsdb_text)

        ted_state = text_field(ted_summary, "state")
        ted_generation = int_field(ted_summary, "generation")
        ted_local_node = text_field(ted_summary, "local node")
        ted_local_group = int_field(ted_summary, "local group")
        ted_node_count = int_field(ted_summary, "nodes")
        ted_intra_count = int_field(ted_summary, "intra links")
        ted_egress_count = int_field(ted_summary, "egress links")
        ted_node_prefix_count = int_field(ted_summary, "node-prefixes")
        ted_group_edge_count = int_field(ted_summary, "group edges")
        ted_prefix_group_count = int_field(ted_summary, "prefix-groups")
        ted_pending = sum(
            int_field(ted_summary, label) or 0
            for label in ("pending links", "pending node-prefixes", "pending prefix-groups")
        )

        memberships = lsdb_memberships(objects)
        local_memberships = lsdb_local_memberships(objects, group_id or -1)
        local_nodes = {row[0] for row in local_memberships}
        links = lsdb_links(objects, local_nodes)
        node_prefixes = lsdb_node_prefixes(objects)
        group_prefixes = lsdb_group_prefixes(objects)
        ted_node_rows = ted_nodes(ted_text)
        ted_link_rows, egress_rows = ted_links(ted_text)
        ted_node_prefix_rows = ted_node_prefixes(ted_text)
        ted_prefix_group_rows = ted_prefix_groups(ted_text)
        ted_edge_rows = ted_group_edges(ted_text)
        all_memberships[node] = memberships
        all_prefix_groups[node] = ted_prefix_group_rows

        checks.check(
            router_id is not None and group_id not in (None, 0),
            f"{node} has a non-zero first-group identity",
        )
        checks.check(
            reported_node == 1 and pending_node == 0 and pending_links == 0,
            f"{node} first-group reports Node/Link facts without pending debt",
        )
        checks.check(
            "\u56de\u8c03\u6ce8\u518c" in interface
            and "\u5df2\u6ce8\u518c" in interface
            and "\u4e24\u4fa7\u4e00\u81f4" in remote_view,
            f"{node} has a registered callback and matching remote view",
        )
        checks.check(
            snapshot_status is not None
            and int(snapshot_status.group(1)) == 0
            and snapshot_nodes == 1
            and snapshot_links == reported_links,
            f"{node} Provider snapshot matches reported first-group facts",
        )
        checks.check(
            len(active_node_lines) == 1
            and len(active_link_lines) == reported_links
            and fact_link_entries >= reported_links,
            f"{node} Local Fact active rows match the Provider snapshot (tombstones retained separately)",
        )
        checks.check(
            owned_memberships == 1 and owned_links == reported_links,
            f"{node} Owned Membership/Link objects match active Local Facts",
        )
        checks.check(
            re.search(r"^\s*state:\s+NORMAL\s*$", input_sync, re.MULTILINE)
            is not None
            and re.search(r"^\s*provider:\s+available\s*$", input_sync, re.MULTILINE)
            is not None
            and re.search(r"^\s*normal queue:\s+0/4096\s*$", input_sync, re.MULTILINE)
            is not None
            and re.search(r"^\s*resync queue:\s+0/4096\s*$", input_sync, re.MULTILINE)
            is not None
            and re.search(r"rejected-full:\s+0", events) is not None,
            f"{node} input store is synchronized with empty queues",
        )
        checks.check(
            owned_sequence_failures == 0 and owned_fightbacks == 0,
            f"{node} owned-object reconciliation has no failure or fightback",
        )
        checks.check(
            identities > 0
            and selected == identities
            and rib_path_count is not None
            and rib_path_count >= selected
            and conflicts == 0
            and payload_conflicts == 0,
            f"{node} RIB selects one conflict-free path per identity",
        )
        checks.check(
            len(remote_selected) > 0 and len(multihop_selected) > 0,
            f"{node} contains selected remote objects with multi-hop propagation paths",
        )
        checks.check(
            lsdb_objects_count == selected
            and lsdb_usable == lsdb_objects_count
            and lsdb_pending == 0
            and lsdb_dirty == 0
            and lsdb_commits is not None
            and lsdb_commits[0] > 0
            and lsdb_commits[1] == 0,
            f"{node} selected RIB identities are atomically committed as usable LSDB objects",
        )
        checks.check(
            lsdb_membership_count == len(MEMBERS) == len(memberships),
            f"{node} LSDB contains all seven propagated Membership objects",
        )
        checks.check(
            ted_state == "READY"
            and (ted_generation or 0) > 0
            and ted_pending == 0
            and ted_local_node == router_id
            and ted_local_group == group_id
            and ted_node_count == expected_size,
            f"{node} maintains a READY TED for its {expected_size}-node group",
        )
        checks.check(
            local_memberships == ted_node_rows,
            f"{node} TED node array matches local-group LSDB Membership objects",
        )
        checks.check(
            links == ted_link_rows
            and len(ted_link_rows) == (ted_intra_count or 0) + (ted_egress_count or 0),
            f"{node} TED directed links and costs match usable LSDB Link objects",
        )
        checks.check(
            node_prefixes == ted_node_prefix_rows
            and len(ted_node_prefix_rows) == ted_node_prefix_count,
            f"{node} TED Node Prefix rows match usable LSDB Node Prefix objects",
        )
        checks.check(
            group_prefixes == ted_prefix_group_rows
            and len(ted_prefix_group_rows) == ted_prefix_group_count,
            f"{node} TED Prefix-to-Group rows match usable Group Prefix objects",
        )
        checks.check(
            expected_group_edges(objects, memberships) == ted_edge_rows
            and len(ted_edge_rows) == ted_group_edge_count,
            f"{node} TED directed Group Edges are the minimum-cost egress aggregation",
        )

        summaries.append(
            {
                "node": node,
                "router_id": router_id or "",
                "group_id": group_id if group_id is not None else -1,
                "reported_links": reported_links,
                "owned_links": owned_links if owned_links is not None else -1,
                "rib_identities": identities,
                "rib_paths": rib_path_count if rib_path_count is not None else -1,
                "rib_selected": selected if selected is not None else -1,
                "remote_selected": len(remote_selected),
                "multihop_selected": len(multihop_selected),
                "lsdb_objects": lsdb_objects_count if lsdb_objects_count is not None else -1,
                "lsdb_usable": lsdb_usable if lsdb_usable is not None else -1,
                "ted_generation": ted_generation if ted_generation is not None else -1,
                "ted_nodes": ted_node_count if ted_node_count is not None else -1,
                "ted_intra_links": ted_intra_count if ted_intra_count is not None else -1,
                "ted_egress_links": ted_egress_count if ted_egress_count is not None else -1,
                "ted_node_prefixes": ted_node_prefix_count
                if ted_node_prefix_count is not None
                else -1,
                "ted_group_edges": ted_group_edge_count
                if ted_group_edge_count is not None
                else -1,
                "ted_prefix_groups": ted_prefix_group_count
                if ted_prefix_group_count is not None
                else -1,
            }
        )
        semantic_rows.append(
            {
                "node": node,
                "memberships": len(memberships),
                "memberships_sha256": digest(memberships),
                "links": len(ted_link_rows),
                "links_sha256": digest(ted_link_rows),
                "node_prefixes": len(ted_node_prefix_rows),
                "node_prefixes_sha256": digest(ted_node_prefix_rows),
                "prefix_groups": len(ted_prefix_group_rows),
                "prefix_groups_sha256": digest(ted_prefix_group_rows),
            }
        )

    membership_sets = list(all_memberships.values())
    prefix_group_sets = list(all_prefix_groups.values())
    checks.check(
        bool(membership_sets)
        and len(membership_sets[0]) == len(MEMBERS)
        and all(rows == membership_sets[0] for rows in membership_sets[1:]),
        "all MIDR members converge on the same global Membership set",
    )
    checks.check(
        bool(prefix_group_sets)
        and len(prefix_group_sets[0]) > 0
        and all(rows == prefix_group_sets[0] for rows in prefix_group_sets[1:]),
        "all MIDR members converge on the same global Prefix-to-Group set",
    )

    with (root / "summary.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(summaries)
    with (root / "semantic-digests.tsv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(semantic_rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(semantic_rows)

    with (root / "summary.md").open("w", encoding="utf-8") as stream:
        stream.write("# MIDR Group1-to-Group2 Evidence\n\n")
        stream.write(f"Verification: {checks.passed} passed, {checks.failed} failed.\n\n")
        stream.write(
            "| Node | Router ID | Group | Provider/Owned Links | "
            "RIB Selected/Paths | Remote/Multi-hop | LSDB Usable | "
            "TED Gen | TED Nodes/Links | Node/Group Prefixes |\n"
        )
        stream.write(
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n"
        )
        for row in summaries:
            stream.write(
                f"| {row['node']} | {row['router_id']} | {row['group_id']} | "
                f"{row['reported_links']}/{row['owned_links']} | "
                f"{row['rib_selected']}/{row['rib_paths']} | "
                f"{row['remote_selected']}/{row['multihop_selected']} | "
                f"{row['lsdb_usable']} | {row['ted_generation']} | "
                f"{row['ted_nodes']}/"
                f"{int(row['ted_intra_links']) + int(row['ted_egress_links'])} | "
                f"{row['ted_node_prefixes']}/{row['ted_prefix_groups']} |\n"
            )
        stream.write("\n")
        stream.write(
            "The checks stop at production TED generation. Path computation, "
            "traffic engineering and route installation are outside this evidence.\n"
        )

    print(f"Result: {checks.passed} passed, {checks.failed} failed")
    return 0 if checks.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
