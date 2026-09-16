#!/usr/bin/env python3
"""
Fetch CAIDA RouteViews pfx2as snapshots and write normalized IP-to-ASN files.

Outputs:
  <output-dir>/raw/<family>/<YYYY>/<MM>/<routeviews-*.pfx2as.gz>
  <output-dir>/snapshots/<snapshot-id>/prefix_asn.txt
  <output-dir>/snapshots/<snapshot-id>/caida_style.txt
  <output-dir>/snapshots/<snapshot-id>/manifest.json
  <output-dir>/latest/prefix_asn.txt
  <output-dir>/latest/caida_style.txt
  <output-dir>/latest/manifest.json
"""

from __future__ import annotations

import argparse
import gzip
import ipaddress
import json
import os
import re
import shutil
import sys
import tempfile
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


DATASETS = {
    "ipv4": {
        "base_url": "https://publicdata.caida.org/datasets/routing/routeviews-prefix2as/",
        "creation_log": "pfx2as-creation.log",
    },
    "ipv6": {
        "base_url": "https://publicdata.caida.org/datasets/routing/routeviews6-prefix2as/",
        "creation_log": "pfx2as-creation.log",
    },
}

LOG_ENTRY_RE = re.compile(
    r"(?:^|\s)(?P<seqnum>\d+)\s+"
    r"(?P<generated_ts>\d{10})\s+"
    r"(?P<path>(?:\d{4}/\d{2}/)?routeviews-[^\s]+\.pfx2as\.gz)"
    r"(?=\s|$)"
)
FILENAME_DATE_RE = re.compile(r"-(?P<date>\d{8})-\d{4}\.pfx2as\.gz$")
LEADING_DECIMAL_ASN_RE = re.compile(r"^\{?(?P<asn>\d+)")


@dataclass(frozen=True)
class LogEntry:
    family: str
    seqnum: int
    generated_ts: int
    path: str
    url: str
    snapshot_date: str


@dataclass
class NormalizeStats:
    family: str
    input_path: str
    total_lines: int = 0
    written_lines: int = 0
    skipped_lines: int = 0
    moas_or_as_set_simplified: int = 0


def fetch_text(url: str, timeout: int) -> str:
    request = Request(url, headers={"User-Agent": "prefix2as-fetcher/1.0"})
    with urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def download_file(url: str, target: Path, timeout: int, force: bool = False) -> None:
    if target.exists() and not force:
        return

    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix=target.name + ".", suffix=".tmp", dir=str(target.parent)
    )
    os.close(fd)
    tmp_path = Path(tmp_name)

    try:
        request = Request(url, headers={"User-Agent": "prefix2as-fetcher/1.0"})
        with urlopen(request, timeout=timeout) as response, tmp_path.open("wb") as out:
            shutil.copyfileobj(response, out)
        tmp_path.replace(target)
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def parse_creation_log(family: str, text: str) -> list[LogEntry]:
    base_url = DATASETS[family]["base_url"]
    entries: list[LogEntry] = []

    for match in LOG_ENTRY_RE.finditer(text):
        path = match.group("path")
        date_match = FILENAME_DATE_RE.search(path)
        if not date_match:
            continue

        entries.append(
            LogEntry(
                family=family,
                seqnum=int(match.group("seqnum")),
                generated_ts=int(match.group("generated_ts")),
                path=path,
                url=urljoin(base_url, path),
                snapshot_date=date_match.group("date"),
            )
        )

    if not entries:
        raise ValueError(f"No pfx2as entries found in {family} creation log")
    return entries


def normalize_requested_date(value: str) -> str:
    if value == "latest":
        return value
    if re.fullmatch(r"\d{8}", value):
        return value
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        return value.replace("-", "")
    raise ValueError("--date must be 'latest', YYYYMMDD, or YYYY-MM-DD")


def select_entry(entries: list[LogEntry], requested_date: str) -> LogEntry:
    if requested_date == "latest":
        return max(entries, key=lambda item: item.seqnum)

    dated_entries = [item for item in entries if item.snapshot_date == requested_date]
    if not dated_entries:
        available = ", ".join(sorted({item.snapshot_date for item in entries})[-10:])
        raise ValueError(
            f"No snapshot for {requested_date}; latest available dates in log: {available}"
        )
    return max(dated_entries, key=lambda item: item.seqnum)


def simplify_asn(raw_asn: str) -> tuple[str | None, bool]:
    match = LEADING_DECIMAL_ASN_RE.match(raw_asn.strip())
    if not match:
        return None, False

    simplified = match.group("asn")
    was_complex = raw_asn != simplified
    return simplified, was_complex


def normalize_pfx2as(
    family: str,
    gz_path: Path,
    prefix_asn_out,
    caida_style_out,
) -> NormalizeStats:
    stats = NormalizeStats(family=family, input_path=str(gz_path))

    with gzip.open(gz_path, "rt", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            stats.total_lines += 1
            parts = line.split()
            if len(parts) < 3:
                stats.skipped_lines += 1
                continue

            prefix, length, raw_asn = parts[0], parts[1], parts[2]
            asn, was_complex = simplify_asn(raw_asn)
            if asn is None:
                stats.skipped_lines += 1
                continue

            try:
                network = ipaddress.ip_network(f"{prefix}/{length}", strict=False)
            except ValueError:
                stats.skipped_lines += 1
                continue

            prefix_asn_out.write(f"{network.with_prefixlen} {asn}\n")
            caida_style_out.write(f"{network.network_address} {network.prefixlen} {asn}\n")
            stats.written_lines += 1
            if was_complex:
                stats.moas_or_as_set_simplified += 1

    return stats


def atomic_write_text(target: Path, text: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = target.with_suffix(target.suffix + ".tmp")
    tmp_path.write_text(text, encoding="utf-8", newline="\n")
    tmp_path.replace(target)


def atomic_copy_file(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = target.with_suffix(target.suffix + ".tmp")
    shutil.copyfile(source, tmp_path)
    tmp_path.replace(target)


def publish_latest(snapshot_dir: Path, latest_dir: Path) -> None:
    for name in ("prefix_asn.txt", "caida_style.txt", "manifest.json"):
        atomic_copy_file(snapshot_dir / name, latest_dir / name)


def make_snapshot_id(entries: Iterable[LogEntry]) -> str:
    parts = []
    for entry in sorted(entries, key=lambda item: item.family):
        parts.append(f"{entry.family}-{entry.snapshot_date}-seq{entry.seqnum}")
    return "__".join(parts)


def build_manifest(entries: list[LogEntry], stats: list[NormalizeStats]) -> dict:
    return {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source": "CAIDA RouteViews Prefix to AS mappings Dataset (pfx2as)",
        "source_catalog_url": "https://www.caida.org/catalog/datasets/routeviews-prefix2as/",
        "families": [entry.family for entry in entries],
        "snapshots": [asdict(entry) for entry in entries],
        "normalization": {
            "prefix_asn": "IP-prefix/prefix-length ASN",
            "caida_style": "IP-prefix prefix-length ASN",
            "asn_rule": "For MOAS/AS-set strings, keep the first leading decimal ASN.",
        },
        "stats": [asdict(item) for item in stats],
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch CAIDA pfx2as snapshots and write normalized IP-to-ASN files."
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/prefix2as"),
        help="Directory for raw snapshots, normalized files, and manifest.",
    )
    parser.add_argument(
        "--families",
        nargs="+",
        choices=sorted(DATASETS),
        default=sorted(DATASETS),
        help="Address families to fetch.",
    )
    parser.add_argument(
        "--date",
        default="latest",
        help="Snapshot date: latest, YYYYMMDD, or YYYY-MM-DD.",
    )
    parser.add_argument(
        "--force-download",
        action="store_true",
        help="Re-download .gz snapshots even if they already exist locally.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Resolve creation logs and print selected snapshot URLs without downloading.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="HTTP timeout in seconds.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    requested_date = normalize_requested_date(args.date)
    output_dir: Path = args.output_dir

    selected_entries: list[LogEntry] = []
    try:
        for family in args.families:
            base_url = DATASETS[family]["base_url"]
            log_url = urljoin(base_url, DATASETS[family]["creation_log"])
            log_text = fetch_text(log_url, timeout=args.timeout)
            entry = select_entry(parse_creation_log(family, log_text), requested_date)
            selected_entries.append(entry)

        snapshot_id = make_snapshot_id(selected_entries)
        if args.dry_run:
            print(f"Selected snapshot id: {snapshot_id}")
            for entry in selected_entries:
                print(f"{entry.family}: seq={entry.seqnum} date={entry.snapshot_date} url={entry.url}")
            return 0

        snapshot_dir = output_dir / "snapshots" / snapshot_id
        raw_paths: dict[str, Path] = {}

        for entry in selected_entries:
            raw_path = output_dir / "raw" / entry.family / entry.path
            download_file(
                entry.url,
                raw_path,
                timeout=args.timeout,
                force=args.force_download,
            )
            raw_paths[entry.family] = raw_path

        snapshot_dir.mkdir(parents=True, exist_ok=True)
        prefix_asn_path = snapshot_dir / "prefix_asn.txt"
        caida_style_path = snapshot_dir / "caida_style.txt"
        stats: list[NormalizeStats] = []

        with prefix_asn_path.open("w", encoding="utf-8", newline="\n") as prefix_out:
            with caida_style_path.open("w", encoding="utf-8", newline="\n") as caida_out:
                prefix_out.write("# prefix/asn\n")
                caida_out.write("# CAIDA prefix2as style\n")
                for entry in selected_entries:
                    stats.append(
                        normalize_pfx2as(
                            entry.family,
                            raw_paths[entry.family],
                            prefix_out,
                            caida_out,
                        )
                    )

        manifest = build_manifest(selected_entries, stats)
        atomic_write_text(
            snapshot_dir / "manifest.json",
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        )
        publish_latest(snapshot_dir, output_dir / "latest")

        print(f"Wrote snapshot: {snapshot_dir}")
        print(f"Wrote latest copy: {output_dir / 'latest'}")
        for item in stats:
            print(
                f"{item.family}: {item.written_lines} rows, "
                f"{item.moas_or_as_set_simplified} simplified, "
                f"{item.skipped_lines} skipped"
            )
        return 0
    except (HTTPError, URLError, TimeoutError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
