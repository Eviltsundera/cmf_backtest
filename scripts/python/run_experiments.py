#!/usr/bin/env python3
"""Run baseline and sensitivity experiments for the LOB backtester."""

from __future__ import annotations

import argparse
import csv
import html
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_GAMMAS = (0.005, 0.01, 0.02)
DEFAULT_KS = (0.5, 1.0, 2.0)
DEFAULT_BETAS = (0.0, 0.5, 1.0)

SUMMARY_METRICS = (
    "final_pnl",
    "max_drawdown",
    "mean_inventory",
    "max_inventory",
    "turnover_qty",
    "fill_rate",
    "avg_spread_captured",
    "adverse_1s",
    "adverse_10s",
    "quote_uptime",
)

ADVERSE_HORIZON_BY_METRIC = {
    "adverse_1s": 1_000_000_000,
    "adverse_10s": 10_000_000_000,
}


@dataclass(frozen=True)
class Experiment:
    name: str
    config: Path
    output_dir: Path
    overrides: tuple[tuple[str, str], ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/lob_backtest"))
    parser.add_argument("--configs-dir", type=Path, default=Path("lob_backtester/configs"))
    parser.add_argument("--reports-dir", type=Path, default=Path("reports"))
    parser.add_argument(
        "--input-path",
        type=Path,
        help="Optional data directory override. Defaults to each YAML config's run.input_path.",
    )
    parser.add_argument("--log-level", default="warn")
    parser.add_argument("--gamma-values", default=join_values(DEFAULT_GAMMAS))
    parser.add_argument("--k-values", default=join_values(DEFAULT_KS))
    parser.add_argument("--beta-values", default=join_values(DEFAULT_BETAS))
    parser.add_argument("--skip-grid", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    gammas = parse_values(args.gamma_values)
    ks = parse_values(args.k_values)
    betas = parse_values(args.beta_values)

    experiments = base_experiments(args)
    if not args.skip_grid:
        experiments.extend(grid_experiments(args, gammas, ks, betas))

    if not args.dry_run:
        args.reports_dir.mkdir(parents=True, exist_ok=True)

    manifest = []
    for experiment in experiments:
        command = command_for(args.binary, experiment)
        manifest.append(
            {
                "name": experiment.name,
                "config": str(experiment.config),
                "output_dir": str(experiment.output_dir),
                "overrides": [{"key": key, "value": value} for key, value in experiment.overrides],
                "command": command,
            }
        )
        print("$ " + " ".join(command), flush=True)
        if not args.dry_run:
            subprocess.run(command, check=True)

    if args.dry_run:
        return 0

    write_json(args.reports_dir / "experiment_manifest.json", {"experiments": manifest})
    write_experiment_summary(args.reports_dir, experiments)
    write_sensitivity_outputs(args.reports_dir)
    return 0


def base_experiments(args: argparse.Namespace) -> list[Experiment]:
    configs_dir = args.configs_dir
    reports_dir = args.reports_dir
    return [
        make_experiment(
            name="baseline_fixed",
            config=configs_dir / "baseline_fixed_spread.yaml",
            output_dir=reports_dir / "baseline_fixed",
            args=args,
        ),
        make_experiment(
            name="avellaneda_stoikov",
            config=configs_dir / "avellaneda_stoikov.yaml",
            output_dir=reports_dir / "avellaneda_stoikov",
            args=args,
        ),
        make_experiment(
            name="microprice_as",
            config=configs_dir / "microprice_as.yaml",
            output_dir=reports_dir / "microprice_as",
            args=args,
        ),
    ]


def grid_experiments(
    args: argparse.Namespace,
    gammas: Iterable[float],
    ks: Iterable[float],
    betas: Iterable[float],
) -> list[Experiment]:
    experiments = []
    config = args.configs_dir / "microprice_as.yaml"
    for gamma in gammas:
        for k_value in ks:
            for beta in betas:
                name = (
                    f"gamma_{label_float(gamma)}__"
                    f"k_{label_float(k_value)}__"
                    f"beta_{label_float(beta)}"
                )
                output_dir = args.reports_dir / "grid_gamma_k_beta" / name
                experiments.append(
                    make_experiment(
                        name=name,
                        config=config,
                        output_dir=output_dir,
                        args=args,
                        extra_overrides=(
                            ("strategy.gamma", format_float(gamma)),
                            ("strategy.k", format_float(k_value)),
                            ("strategy.microprice_beta", format_float(beta)),
                        ),
                    )
                )
    return experiments


def make_experiment(
    *,
    name: str,
    config: Path,
    output_dir: Path,
    args: argparse.Namespace,
    extra_overrides: tuple[tuple[str, str], ...] = (),
) -> Experiment:
    overrides = [("run.output_dir", str(output_dir)), ("run.log_level", args.log_level)]
    if args.input_path is not None:
        overrides.append(("run.input_path", str(args.input_path)))
    overrides.extend(extra_overrides)
    return Experiment(name=name, config=config, output_dir=output_dir, overrides=tuple(overrides))


def command_for(binary: Path, experiment: Experiment) -> list[str]:
    command = [str(binary), "--config", str(experiment.config), "--json"]
    for key, value in experiment.overrides:
        command.extend(["--override", f"{key}={value}"])
    return command


def write_experiment_summary(reports_dir: Path, experiments: Iterable[Experiment]) -> None:
    rows = []
    for experiment in experiments:
        metrics_path = experiment.output_dir / "metrics.json"
        if not metrics_path.exists():
            continue
        metrics = read_json(metrics_path)
        row = {"run": relative_run_name(reports_dir, experiment.output_dir)}
        row.update({metric: metric_value(metrics, metric) for metric in SUMMARY_METRICS})
        rows.append(row)

    lines = [
        "# Experiment Summary",
        "",
        f"- Reports directory: `{reports_dir}`",
        f"- Runs discovered: {len(rows)}",
        "",
        markdown_table(rows, ("run", *SUMMARY_METRICS)),
        "",
    ]
    (reports_dir / "experiment_summary.md").write_text("\n".join(lines), encoding="utf-8")


def write_sensitivity_outputs(reports_dir: Path) -> None:
    rows = load_grid_rows(reports_dir)
    if not rows:
        return

    static_dir = reports_dir / "_static" / "sensitivity"
    static_dir.mkdir(parents=True, exist_ok=True)
    write_csv(static_dir / "grid_metrics.csv", rows)
    write_heatmap_svg(
        static_dir / "final_pnl_gamma_beta.svg",
        rows=rows,
        x_key="gamma",
        y_key="beta",
        metric="final_pnl",
    )
    write_heatmap_svg(
        static_dir / "final_pnl_gamma_k.svg",
        rows=rows,
        x_key="gamma",
        y_key="k",
        metric="final_pnl",
    )


def load_grid_rows(reports_dir: Path) -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    grid_dir = reports_dir / "grid_gamma_k_beta"
    for metadata_path in sorted(grid_dir.glob("*/run_metadata.json")):
        run_dir = metadata_path.parent
        metrics_path = run_dir / "metrics.json"
        if not metrics_path.exists():
            continue
        params = parameters_from_metadata(read_json(metadata_path))
        metrics = read_json(metrics_path)
        if not {"gamma", "k", "microprice_beta"}.issubset(params):
            continue
        rows.append(
            {
                "run": relative_run_name(reports_dir, run_dir),
                "gamma": params["gamma"],
                "k": params["k"],
                "beta": params["microprice_beta"],
                **numeric_metric_values(metrics),
            }
        )
    return rows


def parameters_from_metadata(metadata: dict[str, object]) -> dict[str, float]:
    params = {}
    overrides = metadata.get("overrides", [])
    if not isinstance(overrides, list):
        return params
    for item in overrides:
        if not isinstance(item, dict):
            continue
        raw_key = item.get("key")
        raw_value = item.get("value")
        if not isinstance(raw_key, str) or not isinstance(raw_value, str):
            continue
        try:
            params[raw_key.split(".")[-1]] = float(raw_value)
        except ValueError:
            continue
    return params


def numeric_metric_values(metrics: dict[str, object]) -> dict[str, float]:
    values = {}
    for metric in SUMMARY_METRICS:
        value = metric_value(metrics, metric)
        if isinstance(value, (int, float)):
            values[metric] = float(value)
    return values


def metric_value(metrics: dict[str, object], metric: str) -> object:
    if metric in ADVERSE_HORIZON_BY_METRIC:
        markouts = metrics.get("adverse_selection_h", {})
        if not isinstance(markouts, dict):
            return ""
        return markouts.get(str(ADVERSE_HORIZON_BY_METRIC[metric]), "")
    return metrics.get(metric, "")


def write_heatmap_svg(
    path: Path,
    *,
    rows: list[dict[str, float | str]],
    x_key: str,
    y_key: str,
    metric: str,
) -> None:
    x_values = sorted({float(row[x_key]) for row in rows if x_key in row})
    y_values = sorted({float(row[y_key]) for row in rows if y_key in row})
    matrix = aggregate_matrix(rows, x_key, y_key, metric)
    values = [value for value in matrix.values() if value is not None]
    if not x_values or not y_values or not values:
        return

    cell = 88
    left = 112
    top = 64
    width = left + cell * len(x_values) + 28
    height = top + cell * len(y_values) + 84
    min_value = min(values)
    max_value = max(values)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        (
            f'<text x="{width / 2:.0f}" y="28" text-anchor="middle" '
            'font-family="Arial, sans-serif" font-size="18" font-weight="700">'
            f'{html.escape(metric)} by {html.escape(x_key)} and {html.escape(y_key)}</text>'
        ),
    ]

    for column, x_value in enumerate(x_values):
        x = left + column * cell + cell / 2
        parts.append(
            f'<text x="{x:.0f}" y="{top - 16}" text-anchor="middle" '
            'font-family="Arial, sans-serif" font-size="12">'
            f'{html.escape(format_float(x_value))}</text>'
        )
    for row_index, y_value in enumerate(y_values):
        y = top + row_index * cell + cell / 2
        parts.append(
            f'<text x="{left - 14}" y="{y + 4:.0f}" text-anchor="end" '
            'font-family="Arial, sans-serif" font-size="12">'
            f'{html.escape(format_float(y_value))}</text>'
        )

    parts.append(
        f'<text x="{left + (cell * len(x_values)) / 2:.0f}" y="{height - 18}" '
        'text-anchor="middle" font-family="Arial, sans-serif" font-size="13">'
        f'{html.escape(x_key)}</text>'
    )
    parts.append(
        f'<text x="22" y="{top + (cell * len(y_values)) / 2:.0f}" '
        'text-anchor="middle" font-family="Arial, sans-serif" font-size="13" '
        f'transform="rotate(-90 22 {top + (cell * len(y_values)) / 2:.0f})">'
        f'{html.escape(y_key)}</text>'
    )

    for row_index, y_value in enumerate(y_values):
        for column, x_value in enumerate(x_values):
            x = left + column * cell
            y = top + row_index * cell
            value = matrix.get((x_value, y_value))
            color = value_color(value, min_value, max_value)
            label = "" if value is None else format_float(value)
            parts.append(
                f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" '
                f'fill="{color}" stroke="#ffffff" stroke-width="2"/>'
            )
            parts.append(
                f'<text x="{x + cell / 2:.0f}" y="{y + cell / 2 + 4:.0f}" '
                'text-anchor="middle" font-family="Arial, sans-serif" font-size="12" '
                f'fill="{text_color(color)}">{html.escape(label)}</text>'
            )

    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def aggregate_matrix(
    rows: list[dict[str, float | str]],
    x_key: str,
    y_key: str,
    metric: str,
) -> dict[tuple[float, float], float | None]:
    grouped: dict[tuple[float, float], list[float]] = {}
    for row in rows:
        if not all(key in row for key in (x_key, y_key, metric)):
            continue
        key = (float(row[x_key]), float(row[y_key]))
        grouped.setdefault(key, []).append(float(row[metric]))
    return {
        key: (sum(values) / len(values) if values else None)
        for key, values in grouped.items()
    }


def value_color(value: float | None, min_value: float, max_value: float) -> str:
    if value is None:
        return "#e5e7eb"
    scale = 0.5 if min_value == max_value else (value - min_value) / (max_value - min_value)
    low = (239, 246, 255)
    high = (30, 64, 175)
    rgb = tuple(round(low[index] + (high[index] - low[index]) * scale) for index in range(3))
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def text_color(color: str) -> str:
    red = int(color[1:3], 16)
    green = int(color[3:5], 16)
    blue = int(color[5:7], 16)
    luminance = (0.299 * red) + (0.587 * green) + (0.114 * blue)
    return "#ffffff" if luminance < 128 else "#0f172a"


def markdown_table(rows: list[dict[str, object]], columns: tuple[str, ...]) -> str:
    if not rows:
        return "_No runs available._"
    lines = [
        "| " + " | ".join(columns) + " |",
        "| " + " | ".join("---" for _ in columns) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(format_cell(row.get(column, "")) for column in columns) + " |")
    return "\n".join(lines)


def write_csv(path: Path, rows: list[dict[str, float | str]]) -> None:
    if not rows:
        return
    columns = sorted({key for row in rows for key in row.keys()})
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def read_json(path: Path) -> dict[str, object]:
    with path.open(encoding="utf-8") as handle:
        loaded = json.load(handle)
    if not isinstance(loaded, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return loaded


def write_json(path: Path, value: dict[str, object]) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def relative_run_name(reports_dir: Path, run_dir: Path) -> str:
    try:
        return run_dir.relative_to(reports_dir).as_posix()
    except ValueError:
        return run_dir.as_posix()


def parse_values(raw: str) -> tuple[float, ...]:
    values = tuple(float(item.strip()) for item in raw.split(",") if item.strip())
    if not values:
        raise ValueError("at least one grid value is required")
    return values


def join_values(values: Iterable[float]) -> str:
    return ",".join(format_float(value) for value in values)


def label_float(value: float) -> str:
    return format_float(value).replace("-", "m").replace(".", "p")


def format_float(value: float) -> str:
    return f"{value:.12g}"


def format_cell(value: object) -> str:
    if isinstance(value, float):
        return format_float(value)
    return str(value)


if __name__ == "__main__":
    raise SystemExit(main())
