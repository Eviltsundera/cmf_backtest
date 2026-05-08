#!/usr/bin/env python3
"""Export a static markdown + PNG report for one backtest run."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import reporting  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True, help="Run directory with metrics.json.")
    parser.add_argument(
        "--reports-dir",
        type=Path,
        default=None,
        help="Optional reports root used to compute reports/_static/<run>.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Optional explicit output directory for summary.md and plots/.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = reporting.export_static(
        args.run,
        reports_dir=args.reports_dir,
        output_dir=args.output_dir,
    )
    print(f"Wrote static report to {output_dir}")


if __name__ == "__main__":
    main()
