#!/usr/bin/env python3
"""Build a local submission directory from source, docs, sample data, and reports."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


BASE_RUNS = ("baseline_fixed", "avellaneda_stoikov", "microprice_as")
REQUIRED_RUN_FILES = (
    "run_metadata.json",
    "metrics.json",
    "equity_curve.csv",
    "inventory.csv",
    "orders.csv",
    "fills.csv",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("submission/cmf_lob_backtester"))
    parser.add_argument("--reports-dir", type=Path, default=Path("reports"))
    parser.add_argument("--include-grid", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path.cwd()
    output = args.output

    validate_inputs(root, args.reports_dir)
    reset_output(output)

    copy_file(root / "README.md", output / "PROJECT_README.md")
    copy_file(root / "requirements.txt", output / "requirements.txt")
    copy_file(root / "CMakeLists.txt", output / "CMakeLists.txt", missing_ok=True)
    copy_tree(root / "docs", output / "docs")
    copy_tree(root / "lob_backtester", output / "lob_backtester", ignore=ignore_backtester_paths)
    copy_tree(root / "scripts/python", output / "scripts/python", ignore=ignore_python_cache)
    copy_tree(root / "data/sample", output / "data/sample")
    copy_tree(root / "data/sample_reports", output / "data/sample_reports", ignore=ignore_sample_report_paths)

    package_reports(args.reports_dir, output / "reports", include_grid=args.include_grid)
    write_submission_readme(output)
    write_manifest(output, args.reports_dir, include_grid=args.include_grid)
    print(f"Wrote submission package to {output}")
    return 0


def validate_inputs(root: Path, reports_dir: Path) -> None:
    required_paths = [
        root / "README.md",
        root / "requirements.txt",
        root / "docs/report.md",
        root / "docs/assets/final_pnl_gamma_beta.svg",
        root / "docs/assets/final_pnl_gamma_k.svg",
        root / "lob_backtester/CMakeLists.txt",
        root / "lob_backtester/src",
        root / "lob_backtester/tests",
        root / "lob_backtester/configs",
        root / "data/sample",
        reports_dir / "experiment_summary.md",
        reports_dir / "_static/sensitivity/final_pnl_gamma_beta.svg",
        reports_dir / "_static/sensitivity/final_pnl_gamma_k.svg",
    ]
    for path in required_paths:
        if not path.exists():
            raise FileNotFoundError(f"required submission input is missing: {path}")

    for run in BASE_RUNS:
        run_dir = reports_dir / run
        for filename in REQUIRED_RUN_FILES:
            path = run_dir / filename
            if not path.exists():
                raise FileNotFoundError(f"required run artifact is missing: {path}")


def reset_output(output: Path) -> None:
    if output.exists():
        if len(output.parts) < 2 or output.parts[0] != "submission":
            raise ValueError("refusing to overwrite output outside submission/")
        shutil.rmtree(output)
    output.mkdir(parents=True)


def package_reports(reports_dir: Path, output: Path, *, include_grid: bool) -> None:
    output.mkdir(parents=True, exist_ok=True)
    copy_file(reports_dir / "experiment_summary.md", output / "experiment_summary.md")
    copy_file(reports_dir / "experiment_manifest.json", output / "experiment_manifest.json")
    copy_file(Path("docs/report.md"), output / "report.md")
    copy_tree(reports_dir / "_static/sensitivity", output / "charts/sensitivity")

    runs_output = output / "runs"
    for run in BASE_RUNS:
        copy_tree(reports_dir / run, runs_output / run)

    if include_grid:
        copy_tree(reports_dir / "grid_gamma_k_beta", output / "grid_gamma_k_beta")


def write_submission_readme(output: Path) -> None:
    content = """# CMF LOB Backtester Submission

This directory is a self-contained submission bundle generated from the
repository.

## Contents

- `lob_backtester/` - C++ source, configs, tests, and CMake project.
- `data/sample/` - committed one-hour sample dataset.
- `reports/report.md` - final performance report.
- `reports/runs/` - base run artifacts for fixed spread, A-S, and microprice A-S.
- `reports/charts/` - sensitivity charts.
- `docs/` - technical documentation and implementation plan.
- `scripts/python/` - experiment, dashboard, static export, and packaging tools.

## Build And Test

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run A Strategy

```bash
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml
```

## Reproduce Experiments

```bash
python3 scripts/python/run_experiments.py
```

## View Results

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

The final report is `reports/report.md`.
"""
    (output / "README.md").write_text(content, encoding="utf-8")


def write_manifest(output: Path, reports_dir: Path, *, include_grid: bool) -> None:
    runs = {}
    for run in BASE_RUNS:
        metrics = json.loads((reports_dir / run / "metrics.json").read_text(encoding="utf-8"))
        metadata = json.loads((reports_dir / run / "run_metadata.json").read_text(encoding="utf-8"))
        runs[run] = {
            "final_pnl": metrics.get("final_pnl"),
            "fill_count": metrics.get("fill_count"),
            "config_hash": metadata.get("config_hash"),
            "git_commit": metadata.get("git_commit"),
        }

    manifest = {
        "package": "cmf_lob_backtester",
        "reports_dir": str(reports_dir),
        "base_runs": runs,
        "includes_grid": include_grid,
    }
    (output / "submission_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def copy_file(src: Path, dst: Path, *, missing_ok: bool = False) -> None:
    if missing_ok and not src.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src: Path, dst: Path, *, ignore=None) -> None:
    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


def ignore_python_cache(_: str, names: list[str]) -> set[str]:
    return {name for name in names if name in {"__pycache__", ".pytest_cache", ".venv"}}


def ignore_sample_report_paths(_: str, names: list[str]) -> set[str]:
    return {name for name in names if name == "_static"}


def ignore_backtester_paths(_: str, names: list[str]) -> set[str]:
    ignored = {"build", "artifacts"}
    ignored.update(name for name in names if name in {"__pycache__", ".pytest_cache", ".venv"})
    return ignored


if __name__ == "__main__":
    raise SystemExit(main())
