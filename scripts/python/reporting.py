#!/usr/bin/env python3
"""Shared readers, transformations, and Plotly figures for backtest reports."""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable

import pandas as pd
import plotly.graph_objects as go


DEFAULT_REPORTS_DIR = Path("reports")
FALLBACK_REPORTS_DIR = Path("lob_backtester/artifacts/runs")

METRIC_ORDER = [
    "final_pnl",
    "mean_inventory",
    "max_inventory",
    "inventory_std",
    "turnover_qty",
    "turnover_notional",
    "fill_count",
    "fill_rate",
    "max_drawdown",
    "avg_quoted_spread",
    "avg_spread_captured",
    "quote_uptime",
]

HIGHER_IS_BETTER = {
    "final_pnl",
    "fill_rate",
    "avg_spread_captured",
    "quote_uptime",
}

LOWER_IS_BETTER = {
    "max_drawdown",
    "inventory_std",
}


@dataclass(frozen=True)
class RunArtifacts:
    name: str
    path: Path
    metrics: dict[str, Any]
    metadata: dict[str, Any]
    equity: pd.DataFrame
    inventory: pd.DataFrame
    orders: pd.DataFrame
    fills: pd.DataFrame


def resolve_reports_dir(requested: Path) -> Path:
    """Use the requested reports dir, falling back to committed sample artifacts."""
    requested = requested.expanduser()
    if requested.exists():
        return requested
    if requested == DEFAULT_REPORTS_DIR and FALLBACK_REPORTS_DIR.exists():
        return FALLBACK_REPORTS_DIR
    return requested


def discover_run_dirs(reports_dir: Path) -> list[Path]:
    reports_dir = reports_dir.expanduser()
    if not reports_dir.exists():
        return []
    runs = []
    for metrics_path in reports_dir.rglob("metrics.json"):
        if "_static" in metrics_path.parts:
            continue
        runs.append(metrics_path.parent)
    return sorted(set(runs), key=lambda path: run_label(path, reports_dir))


def run_label(run_dir: Path, reports_dir: Path) -> str:
    try:
        return run_dir.relative_to(reports_dir).as_posix()
    except ValueError:
        return run_dir.name


def load_run(run_dir: Path, reports_dir: Path | None = None) -> RunArtifacts:
    run_dir = run_dir.expanduser()
    reports_dir = reports_dir.expanduser() if reports_dir is not None else run_dir.parent
    return RunArtifacts(
        name=run_label(run_dir, reports_dir),
        path=run_dir,
        metrics=read_json(run_dir / "metrics.json"),
        metadata=read_json(run_dir / "run_metadata.json", missing_ok=True),
        equity=read_timeseries_csv(run_dir / "equity_curve.csv"),
        inventory=read_timeseries_csv(run_dir / "inventory.csv"),
        orders=read_timeseries_csv(run_dir / "orders.csv"),
        fills=read_timeseries_csv(run_dir / "fills.csv"),
    )


def read_json(path: Path, *, missing_ok: bool = False) -> dict[str, Any]:
    if missing_ok and not path.exists():
        return {}
    with path.open(encoding="utf-8") as handle:
        loaded = json.load(handle)
    if not isinstance(loaded, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return loaded


def read_timeseries_csv(path: Path) -> pd.DataFrame:
    if not path.exists() or path.stat().st_size == 0:
        return pd.DataFrame()
    frame = pd.read_csv(path)
    if "ts_ns" in frame.columns:
        frame["timestamp"] = pd.to_datetime(frame["ts_ns"], unit="ns", utc=True, errors="coerce")
    return frame


def filter_run_by_time(
    run: RunArtifacts,
    start: pd.Timestamp | None,
    end: pd.Timestamp | None,
) -> RunArtifacts:
    if start is None or end is None:
        return run
    return replace(
        run,
        equity=filter_frame_by_time(run.equity, start, end),
        inventory=filter_frame_by_time(run.inventory, start, end),
        orders=filter_frame_by_time(run.orders, start, end),
        fills=filter_frame_by_time(run.fills, start, end),
    )


def filter_frame_by_time(
    frame: pd.DataFrame,
    start: pd.Timestamp,
    end: pd.Timestamp,
) -> pd.DataFrame:
    if frame.empty or "timestamp" not in frame.columns:
        return frame
    mask = frame["timestamp"].between(start, end, inclusive="both")
    return frame.loc[mask].copy()


def combined_time_bounds(runs: Iterable[RunArtifacts]) -> tuple[pd.Timestamp | None, pd.Timestamp | None]:
    starts = []
    ends = []
    for run in runs:
        frame = run.equity
        if frame.empty or "timestamp" not in frame.columns:
            continue
        starts.append(frame["timestamp"].min())
        ends.append(frame["timestamp"].max())
    if not starts or not ends:
        return None, None
    return min(starts), max(ends)


def metrics_table(runs: Iterable[RunArtifacts]) -> pd.DataFrame:
    rows: dict[str, dict[str, float]] = {}
    for run in runs:
        for key, value in run.metrics.items():
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                continue
            if not math.isfinite(float(value)):
                continue
            rows.setdefault(key, {})[run.name] = float(value)

    ordered = [key for key in METRIC_ORDER if key in rows]
    ordered.extend(sorted(key for key in rows if key not in ordered))
    return pd.DataFrame.from_dict({key: rows[key] for key in ordered}, orient="index")


def metadata_table(runs: Iterable[RunArtifacts]) -> pd.DataFrame:
    rows = []
    for run in runs:
        metadata = run.metadata
        rows.append(
            {
                "run": run.name,
                "config_hash": metadata.get("config_hash", "unknown"),
                "git_commit": metadata.get("git_commit", "unknown"),
                "timestamp_utc": metadata.get("timestamp_utc", "unknown"),
                "config_path": metadata.get("config_path", "unknown"),
                "overrides": format_overrides(metadata.get("overrides", [])),
            }
        )
    return pd.DataFrame(rows).set_index("run") if rows else pd.DataFrame()


def format_overrides(overrides: Any) -> str:
    if not isinstance(overrides, list):
        return ""
    formatted = []
    for item in overrides:
        if isinstance(item, dict) and "key" in item and "value" in item:
            formatted.append(f"{item['key']}={item['value']}")
    return ", ".join(formatted)


def style_metric_table(frame: pd.DataFrame) -> pd.io.formats.style.Styler:
    def highlight(row: pd.Series) -> list[str]:
        numeric = pd.to_numeric(row, errors="coerce").dropna()
        styles = ["" for _ in row]
        if len(numeric) <= 1:
            return styles

        metric = str(row.name)
        best = numeric.min() if metric in LOWER_IS_BETTER else numeric.max()
        worst = numeric.max() if metric in LOWER_IS_BETTER else numeric.min()
        for idx, value in enumerate(row):
            if not isinstance(value, (int, float)):
                continue
            if math.isclose(float(value), float(best), rel_tol=1e-12, abs_tol=1e-12):
                styles[idx] = "background-color: #d1fae5"
            elif math.isclose(float(value), float(worst), rel_tol=1e-12, abs_tol=1e-12):
                styles[idx] = "background-color: #fee2e2"
        return styles

    return frame.style.format(precision=6).apply(highlight, axis=1)


def equity_figure(runs: Iterable[RunArtifacts], *, normalize: bool = False) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        frame = run.equity
        if frame.empty or "equity" not in frame.columns:
            continue
        y = frame["equity"].astype(float)
        if normalize and not y.empty:
            y = y - y.iloc[0]
        fig.add_trace(go.Scatter(x=frame["timestamp"], y=y, mode="lines", name=run.name))
    return prepare_figure(fig, "Equity", "time", "equity")


def drawdown_figure(runs: Iterable[RunArtifacts], *, normalize: bool = False) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        frame = run.equity
        if frame.empty or "equity" not in frame.columns:
            continue
        equity = frame["equity"].astype(float)
        if normalize and not equity.empty:
            equity = equity - equity.iloc[0]
        drawdown = equity - equity.cummax()
        fig.add_trace(go.Scatter(x=frame["timestamp"], y=drawdown, mode="lines", name=run.name))
    return prepare_figure(fig, "Drawdown", "time", "drawdown")


def inventory_figure(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        frame = run.inventory
        if frame.empty or "position_lots" not in frame.columns:
            continue
        fig.add_trace(
            go.Scatter(x=frame["timestamp"], y=frame["position_lots"], mode="lines", name=run.name)
        )
        observed_limit = run.metrics.get("max_inventory")
        if isinstance(observed_limit, (int, float)) and observed_limit > 0:
            fig.add_hline(
                y=observed_limit,
                line_dash="dot",
                line_color="#64748b",
                annotation_text=f"{run.name} observed max",
            )
            fig.add_hline(y=-observed_limit, line_dash="dot", line_color="#64748b")
    return prepare_figure(fig, "Inventory", "time", "position lots")


def inventory_histogram(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        frame = run.inventory
        if frame.empty or "position_lots" not in frame.columns:
            continue
        fig.add_trace(
            go.Histogram(
                x=frame["position_lots"],
                name=run.name,
                opacity=0.65,
                histnorm="probability",
            )
        )
    fig.update_layout(barmode="overlay")
    return prepare_figure(fig, "Inventory Distribution", "position lots", "probability")


def quote_distance_figure(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        quotes = quote_distances(run)
        if quotes.empty:
            continue
        for side, side_frame in quotes.groupby("side"):
            fig.add_trace(
                go.Scatter(
                    x=side_frame["timestamp"],
                    y=side_frame["distance_to_mid"],
                    mode="markers",
                    marker={"size": 5},
                    name=f"{run.name} {side}",
                )
            )
    return prepare_figure(fig, "Quote Distance To Mid", "time", "ticks from mid")


def quote_spread_figure(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        spreads = quoted_spreads(run)
        if spreads.empty:
            continue
        fig.add_trace(
            go.Scatter(
                x=spreads["timestamp"],
                y=spreads["quoted_spread"],
                mode="lines+markers",
                name=run.name,
            )
        )
    return prepare_figure(fig, "Quoted Spread", "time", "ticks")


def fills_figure(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    for run in runs:
        fills = fills_with_mid(run)
        if fills.empty:
            continue
        for side, side_frame in fills.groupby("side"):
            fig.add_trace(
                go.Scatter(
                    x=side_frame["mark_price"],
                    y=side_frame["fill_price_ticks"],
                    mode="markers",
                    marker={"size": 7},
                    name=f"{run.name} {side}",
                    customdata=side_frame[["timestamp", "quantity_lots", "fee"]],
                    hovertemplate=(
                        "mid=%{x}<br>fill=%{y}<br>ts=%{customdata[0]}"
                        "<br>qty=%{customdata[1]}<br>fee=%{customdata[2]}<extra></extra>"
                    ),
                )
            )
    return prepare_figure(fig, "Fill Price vs Reference Mid", "mid", "fill price")


def adverse_selection_figure(runs: Iterable[RunArtifacts]) -> go.Figure:
    fig = go.Figure()
    has_data = False
    for run in runs:
        markouts = run.metrics.get("adverse_selection_h", {})
        if not isinstance(markouts, dict) or not markouts:
            continue
        has_data = True
        fig.add_trace(
            go.Bar(
                x=[format_horizon(int(horizon)) for horizon in markouts.keys()],
                y=list(markouts.values()),
                name=run.name,
            )
        )
    if not has_data:
        fig.add_annotation(
            text="No adverse-selection horizons were recorded for the selected run(s).",
            xref="paper",
            yref="paper",
            x=0.5,
            y=0.5,
            showarrow=False,
        )
    return prepare_figure(fig, "Adverse Selection Markout", "horizon", "markout")


def quote_distances(run: RunArtifacts) -> pd.DataFrame:
    orders = submitted_orders(run)
    if orders.empty or run.equity.empty or "mark_price" not in run.equity.columns:
        return pd.DataFrame()
    merged = pd.merge_asof(
        orders.sort_values("ts_ns"),
        run.equity[["ts_ns", "mark_price"]].sort_values("ts_ns"),
        on="ts_ns",
        direction="backward",
    )
    merged = merged.dropna(subset=["mark_price", "price_ticks"]).copy()
    side = merged["side"].astype(str).str.lower()
    buy_distance = merged["mark_price"].astype(float) - merged["price_ticks"].astype(float)
    sell_distance = merged["price_ticks"].astype(float) - merged["mark_price"].astype(float)
    merged["distance_to_mid"] = buy_distance.where(side == "buy", sell_distance)
    return merged


def quoted_spreads(run: RunArtifacts) -> pd.DataFrame:
    orders = submitted_orders(run)
    if orders.empty:
        return pd.DataFrame()
    buys = (
        orders[orders["side"].astype(str).str.lower() == "buy"]
        .groupby("ts_ns")["price_ticks"]
        .max()
    )
    sells = (
        orders[orders["side"].astype(str).str.lower() == "sell"]
        .groupby("ts_ns")["price_ticks"]
        .min()
    )
    spreads = pd.concat({"bid": buys, "ask": sells}, axis=1).dropna()
    if spreads.empty:
        return pd.DataFrame()
    spreads["quoted_spread"] = spreads["ask"].astype(float) - spreads["bid"].astype(float)
    spreads["timestamp"] = pd.to_datetime(spreads.index, unit="ns", utc=True, errors="coerce")
    return spreads.reset_index()


def submitted_orders(run: RunArtifacts) -> pd.DataFrame:
    orders = run.orders
    if orders.empty or "event_type" not in orders.columns:
        return pd.DataFrame()
    submitted = orders[orders["event_type"].astype(str).str.lower() == "submitted"].copy()
    return submitted.dropna(subset=["ts_ns", "price_ticks", "side"])


def fills_with_mid(run: RunArtifacts) -> pd.DataFrame:
    fills = run.fills
    if fills.empty or run.equity.empty or "mark_price" not in run.equity.columns:
        return pd.DataFrame()
    merged = pd.merge_asof(
        fills.sort_values("ts_ns"),
        run.equity[["ts_ns", "mark_price"]].sort_values("ts_ns"),
        on="ts_ns",
        direction="backward",
    )
    return merged.dropna(subset=["mark_price", "fill_price_ticks"])


def sensitivity_frame(runs: Iterable[RunArtifacts]) -> pd.DataFrame:
    rows = []
    for run in runs:
        row: dict[str, Any] = {"run": run.name}
        row.update(extract_parameters(run))
        for metric, value in run.metrics.items():
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                row[metric] = float(value)
        rows.append(row)
    return pd.DataFrame(rows)


def extract_parameters(run: RunArtifacts) -> dict[str, float]:
    params: dict[str, float] = {}
    overrides = run.metadata.get("overrides", [])
    if isinstance(overrides, list):
        for item in overrides:
            if not isinstance(item, dict):
                continue
            key = str(item.get("key", "")).split(".")[-1]
            value = parse_float(item.get("value"))
            if key and value is not None:
                params[key] = value

    for key, value in re.findall(r"(gamma|beta|alpha|k)[=_-](-?\d+(?:\.\d+)?)", run.name):
        parsed = parse_float(value)
        if parsed is not None:
            params.setdefault(key, parsed)
    return params


def parse_float(value: Any) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def sensitivity_heatmap(frame: pd.DataFrame, x_param: str, y_param: str, metric: str) -> go.Figure:
    if frame.empty:
        return prepare_figure(go.Figure(), "Sensitivity", x_param, y_param)
    pivot = frame.pivot_table(index=y_param, columns=x_param, values=metric, aggfunc="mean")
    fig = go.Figure(
        data=go.Heatmap(
            z=pivot.values,
            x=list(pivot.columns),
            y=list(pivot.index),
            colorscale="Viridis",
            colorbar={"title": metric},
        )
    )
    return prepare_figure(fig, f"{metric} Sensitivity", x_param, y_param)


def paginated_frame(frame: pd.DataFrame, page: int, page_size: int) -> pd.DataFrame:
    if frame.empty:
        return frame
    start = max(page - 1, 0) * page_size
    return frame.iloc[start : start + page_size]


def summary_markdown(run: RunArtifacts) -> str:
    metadata = metadata_table([run])
    metrics = metrics_table([run])

    lines = [
        f"# Backtest Summary: {run.name}",
        "",
        f"- Run directory: `{run.path}`",
        f"- Config hash: `{run.metadata.get('config_hash', 'unknown')}`",
        f"- Git commit: `{run.metadata.get('git_commit', 'unknown')}`",
        "",
        "## Metrics",
        "",
        dataframe_to_markdown(metrics) if not metrics.empty else "_No metrics available._",
        "",
        "## Metadata",
        "",
        dataframe_to_markdown(metadata) if not metadata.empty else "_No metadata available._",
        "",
        "## Plots",
        "",
        "- `plots/equity.png`",
        "- `plots/drawdown.png`",
        "- `plots/inventory.png`",
        "- `plots/fills.png`",
        "- `plots/adverse_selection.png`",
        "",
    ]
    return "\n".join(lines)


def export_static(
    run_dir: Path,
    *,
    output_dir: Path | None = None,
    reports_dir: Path | None = None,
) -> Path:
    run_dir = run_dir.expanduser()
    reports_dir = reports_dir.expanduser() if reports_dir is not None else infer_reports_dir(run_dir)
    run = load_run(run_dir, reports_dir)
    target = output_dir.expanduser() if output_dir is not None else default_static_dir(run_dir, reports_dir)
    plots_dir = target / "plots"
    plots_dir.mkdir(parents=True, exist_ok=True)

    figures = {
        "equity": equity_figure([run]),
        "drawdown": drawdown_figure([run]),
        "inventory": inventory_figure([run]),
        "fills": fills_figure([run]),
        "adverse_selection": adverse_selection_figure([run]),
    }
    for name, figure in figures.items():
        figure.write_image(plots_dir / f"{name}.png")

    (target / "summary.md").write_text(summary_markdown(run), encoding="utf-8")
    return target


def infer_reports_dir(run_dir: Path) -> Path:
    for parent in run_dir.parents:
        if parent.name in {"reports", "runs"}:
            return parent
    return run_dir.parent


def dataframe_to_markdown(frame: pd.DataFrame) -> str:
    columns = [str(frame.index.name or "metric"), *[str(column) for column in frame.columns]]
    rows = [
        [markdown_cell(index), *[markdown_cell(value) for value in row]]
        for index, row in frame.iterrows()
    ]
    header = "| " + " | ".join(markdown_cell(column) for column in columns) + " |"
    separator = "| " + " | ".join("---" for _ in columns) + " |"
    body = ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join([header, separator, *body])


def markdown_cell(value: Any) -> str:
    if isinstance(value, float):
        value = f"{value:.12g}"
    return str(value).replace("|", "\\|").replace("\n", " ")


def default_static_dir(run_dir: Path, reports_dir: Path) -> Path:
    label = run_label(run_dir, reports_dir).replace("/", "__")
    return reports_dir / "_static" / label


def format_horizon(horizon_ns: int) -> str:
    if horizon_ns % 1_000_000_000 == 0:
        return f"{horizon_ns // 1_000_000_000}s"
    if horizon_ns % 1_000_000 == 0:
        return f"{horizon_ns // 1_000_000}ms"
    return f"{horizon_ns}ns"


def prepare_figure(fig: go.Figure, title: str, x_title: str, y_title: str) -> go.Figure:
    fig.update_layout(
        title=title,
        xaxis_title=x_title,
        yaxis_title=y_title,
        template="plotly_white",
        hovermode="closest",
        legend_title_text="run",
        margin={"l": 48, "r": 24, "t": 56, "b": 48},
        height=420,
    )
    return fig
