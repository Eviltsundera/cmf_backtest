#!/usr/bin/env python3
"""Audit raw market-data CSV files and build a deterministic sample window."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Iterable


HOUR_US = 3_600_000_000
DEFAULT_RAW_DIR = Path("data/raw")
DEFAULT_SAMPLE_DIR = Path("data/sample")


@dataclass
class ColumnProfile:
    name: str
    types: set[str] = field(default_factory=set)
    min_value: float | None = None
    max_value: float | None = None
    example: str | None = None

    def observe(self, value: str) -> None:
        if value == "":
            self.types.add("null")
            return
        if self.example is None:
            self.example = value

        inferred = infer_type(value)
        self.types.add(inferred)
        if inferred in {"int", "float"}:
            number = float(value)
            self.min_value = number if self.min_value is None else min(self.min_value, number)
            self.max_value = number if self.max_value is None else max(self.max_value, number)

    def as_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "types": sorted(self.types),
            "min": self.min_value,
            "max": self.max_value,
            "example": self.example,
        }


@dataclass
class FileAudit:
    path: Path
    size_bytes: int
    event_type: str
    columns: list[str]
    rows: int = 0
    min_ts: int | None = None
    max_ts: int | None = None
    monotonic_violations: int = 0
    side_counts: Counter[str] = field(default_factory=Counter)
    price_min: float | None = None
    price_max: float | None = None
    amount_min: float | None = None
    amount_max: float | None = None
    hourly_counts: Counter[int] = field(default_factory=Counter)
    examples: list[dict[str, str]] = field(default_factory=list)
    column_profiles: list[ColumnProfile] = field(default_factory=list)

    def update_ts(self, ts: int, previous_ts: int | None) -> None:
        self.min_ts = ts if self.min_ts is None else min(self.min_ts, ts)
        self.max_ts = ts if self.max_ts is None else max(self.max_ts, ts)
        self.hourly_counts[ts // HOUR_US] += 1
        if previous_ts is not None and ts < previous_ts:
            self.monotonic_violations += 1

    def as_dict(self) -> dict[str, object]:
        return {
            "file": str(self.path),
            "format": "csv",
            "size_bytes": self.size_bytes,
            "event_type": self.event_type,
            "rows": self.rows,
            "columns": self.columns,
            "timestamp_unit": "microseconds",
            "min_ts": self.min_ts,
            "max_ts": self.max_ts,
            "min_time_utc": format_ts(self.min_ts),
            "max_time_utc": format_ts(self.max_ts),
            "monotonic_violations": self.monotonic_violations,
            "side_counts": dict(self.side_counts),
            "price_min": self.price_min,
            "price_max": self.price_max,
            "amount_min": self.amount_min,
            "amount_max": self.amount_max,
            "examples": self.examples,
            "column_profiles": [profile.as_dict() for profile in self.column_profiles],
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-dir", type=Path, default=DEFAULT_RAW_DIR)
    parser.add_argument("--sample-dir", type=Path, default=DEFAULT_SAMPLE_DIR)
    parser.add_argument(
        "--inspect-rows",
        type=int,
        default=10_000,
        help="Rows per file used for column profiling and examples.",
    )
    parser.add_argument(
        "--sample-start",
        type=parse_sample_start,
        default=None,
        help="UTC sample start, e.g. 2024-08-05T06:00:00Z. Defaults to busiest hour.",
    )
    parser.add_argument(
        "--no-sample",
        action="store_true",
        help="Audit only; do not write sample files.",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="Optional path for machine-readable audit summary.",
    )
    return parser.parse_args()


def parse_sample_start(value: str) -> int:
    normalized = value.removesuffix("Z")
    parsed = datetime.fromisoformat(normalized)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=UTC)
    return int(parsed.astimezone(UTC).timestamp() * 1_000_000)


def infer_type(value: str) -> str:
    try:
        int(value)
    except ValueError:
        pass
    else:
        return "int"

    try:
        float(value)
    except ValueError:
        return "string"
    return "float"


def display_columns(raw_header: Iterable[str]) -> list[str]:
    return [name if name else "row_id" for name in raw_header]


def event_type_for(path: Path, columns: list[str]) -> str:
    if "side" in columns and "price" in columns and "amount" in columns:
        return "trade"
    if any(name.startswith("asks[") for name in columns) and any(
        name.startswith("bids[") for name in columns
    ):
        return "book_snapshot_25"
    return path.stem


def format_ts(ts_us: int | None) -> str | None:
    if ts_us is None:
        return None
    return datetime.fromtimestamp(ts_us / 1_000_000, UTC).isoformat()


def profile_file(path: Path, inspect_rows: int) -> FileAudit:
    with path.open(newline="") as handle:
        reader = csv.reader(handle)
        raw_header = next(reader)
        columns = display_columns(raw_header)
        timestamp_idx = columns.index("local_timestamp")
        side_idx = columns.index("side") if "side" in columns else None
        price_idx = columns.index("price") if "price" in columns else None
        amount_idx = columns.index("amount") if "amount" in columns else None

        audit = FileAudit(
            path=path,
            size_bytes=path.stat().st_size,
            event_type=event_type_for(path, columns),
            columns=columns,
            column_profiles=[ColumnProfile(name=name) for name in columns],
        )
        previous_ts: int | None = None

        for row in reader:
            audit.rows += 1
            ts = int(row[timestamp_idx])
            audit.update_ts(ts, previous_ts)
            previous_ts = ts

            if side_idx is not None:
                audit.side_counts[row[side_idx]] += 1
            if price_idx is not None:
                audit.price_min, audit.price_max = update_range(
                    audit.price_min, audit.price_max, float(row[price_idx])
                )
            if amount_idx is not None:
                audit.amount_min, audit.amount_max = update_range(
                    audit.amount_min, audit.amount_max, float(row[amount_idx])
                )

            if audit.rows <= inspect_rows:
                for idx, value in enumerate(row):
                    audit.column_profiles[idx].observe(value)
                if len(audit.examples) < 3:
                    audit.examples.append(dict(zip(columns, row, strict=True)))

    return audit


def update_range(
    current_min: float | None, current_max: float | None, value: float
) -> tuple[float, float]:
    return (
        value if current_min is None else min(current_min, value),
        value if current_max is None else max(current_max, value),
    )


def raw_csv_files(raw_dir: Path) -> list[Path]:
    return sorted(path for path in raw_dir.glob("*.csv") if path.is_file())


def choose_sample_hour(audits: list[FileAudit]) -> tuple[int, int]:
    combined: Counter[int] = Counter()
    for audit in audits:
        combined.update(audit.hourly_counts)
    if not combined:
        raise RuntimeError("No timestamped rows found; cannot choose sample window.")
    hour_bucket, _ = combined.most_common(1)[0]
    start = hour_bucket * HOUR_US
    return start, start + HOUR_US


def write_sample(
    raw_files: list[Path],
    sample_dir: Path,
    start_us: int,
    end_us: int,
) -> dict[str, object]:
    sample_dir.mkdir(parents=True, exist_ok=True)
    files: dict[str, object] = {}

    for source in raw_files:
        destination = sample_dir / source.name
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        count = 0

        with source.open(newline="") as src, temporary.open("w", newline="") as dst:
            reader = csv.reader(src)
            writer = csv.writer(dst, lineterminator="\n")
            raw_header = next(reader)
            columns = display_columns(raw_header)
            timestamp_idx = columns.index("local_timestamp")
            writer.writerow(raw_header)
            for row in reader:
                ts = int(row[timestamp_idx])
                if start_us <= ts < end_us:
                    writer.writerow(row)
                    count += 1

        temporary.replace(destination)
        files[source.name] = {"rows": count, "size_bytes": destination.stat().st_size}

    manifest = {
        "sample_start_utc": format_ts(start_us),
        "sample_end_utc": format_ts(end_us),
        "sample_duration_seconds": int((end_us - start_us) / 1_000_000),
        "files": files,
    }
    manifest_path = sample_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def print_human_summary(
    audits: list[FileAudit],
    sample_start_us: int,
    sample_end_us: int,
    sample_manifest: dict[str, object] | None,
    sample_dir: Path,
) -> None:
    print("Raw data audit")
    print(f"sample_window_utc: {format_ts(sample_start_us)} -> {format_ts(sample_end_us)}")
    print()

    print("Event counts")
    counts: Counter[str] = Counter()
    for audit in audits:
        counts[audit.event_type] += audit.rows
    for event_type, rows in sorted(counts.items()):
        print(f"- {event_type}: {rows:,}")
    if "trade" not in counts:
        print("- trade: 0")
    if "book_snapshot_25" not in counts:
        print("- book_snapshot_25: 0")
    print("- depth_update: 0 (not present as a native raw event)")
    print()

    for audit in audits:
        print(f"{audit.path}")
        print(f"  format: csv")
        print(f"  size_bytes: {audit.size_bytes:,}")
        print(f"  event_type: {audit.event_type}")
        print(f"  rows: {audit.rows:,}")
        print(f"  columns: {len(audit.columns)} ({column_summary(audit.columns)})")
        print("  key_column_profiles:")
        for profile in key_profiles(audit):
            print(f"    - {format_profile(profile)}")
        print(f"  timestamp_unit: microseconds")
        print(f"  time_range_utc: {format_ts(audit.min_ts)} -> {format_ts(audit.max_ts)}")
        print(f"  monotonic_violations: {audit.monotonic_violations}")
        if audit.side_counts:
            side_counts = ", ".join(
                f"{side}={count:,}" for side, count in sorted(audit.side_counts.items())
            )
            print(f"  side_counts: {side_counts}")
        if audit.price_min is not None:
            print(f"  trade_price_range: {audit.price_min}..{audit.price_max}")
        if audit.amount_min is not None:
            print(f"  trade_amount_range: {audit.amount_min}..{audit.amount_max}")
        top_hours = audit.hourly_counts.most_common(3)
        formatted_hours = ", ".join(
            f"{format_ts(hour * HOUR_US)}={count:,}" for hour, count in top_hours
        )
        print(f"  top_hours: {formatted_hours}")
        print()

    if sample_manifest is not None:
        print("Sample files")
        for name, info in sample_manifest["files"].items():
            print(f"- {name}: rows={info['rows']:,}, size_bytes={info['size_bytes']:,}")
        print(f"- manifest.json: {sample_dir / 'manifest.json'}")


def column_summary(columns: list[str]) -> str:
    if len(columns) <= 12:
        return ", ".join(columns)
    head = ", ".join(columns[:8])
    tail = ", ".join(columns[-4:])
    return f"{head}, ..., {tail}"


def key_profiles(audit: FileAudit) -> list[ColumnProfile]:
    if audit.event_type == "book_snapshot_25":
        names = {
            "row_id",
            "local_timestamp",
            "asks[0].price",
            "asks[0].amount",
            "bids[0].price",
            "bids[0].amount",
            "asks[24].price",
            "asks[24].amount",
            "bids[24].price",
            "bids[24].amount",
        }
    else:
        names = set(audit.columns)
    return [profile for profile in audit.column_profiles if profile.name in names]


def format_profile(profile: ColumnProfile) -> str:
    type_label = "|".join(sorted(profile.types)) if profile.types else "unknown"
    range_label = ""
    if profile.min_value is not None and profile.max_value is not None:
        range_label = f", sampled_range={profile.min_value}..{profile.max_value}"
    example_label = f", example={profile.example}" if profile.example is not None else ""
    return f"{profile.name}: {type_label}{range_label}{example_label}"


def main() -> int:
    args = parse_args()
    raw_files = raw_csv_files(args.raw_dir)
    if not raw_files:
        raise SystemExit(f"No CSV files found in {args.raw_dir}")

    audits = [profile_file(path, args.inspect_rows) for path in raw_files]
    if args.sample_start is None:
        sample_start_us, sample_end_us = choose_sample_hour(audits)
    else:
        sample_start_us = args.sample_start
        sample_end_us = sample_start_us + HOUR_US

    sample_manifest = None
    if not args.no_sample:
        sample_manifest = write_sample(raw_files, args.sample_dir, sample_start_us, sample_end_us)

    summary = {
        "files": [audit.as_dict() for audit in audits],
        "event_counts": {audit.event_type: audit.rows for audit in audits},
        "sample_window_utc": {
            "start": format_ts(sample_start_us),
            "end": format_ts(sample_end_us),
        },
        "sample": sample_manifest,
    }
    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    print_human_summary(audits, sample_start_us, sample_end_us, sample_manifest, args.sample_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
