#!/usr/bin/env python3
"""Streamlit dashboard for CMF LOB backtest run artifacts."""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import pandas as pd
import streamlit as st

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import reporting  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--reports-dir", type=Path, default=reporting.DEFAULT_REPORTS_DIR)
    args, _ = parser.parse_known_args()
    return args


@st.cache_data(show_spinner=False)
def cached_run_dirs(reports_dir: str) -> list[str]:
    return [str(path) for path in reporting.discover_run_dirs(Path(reports_dir))]


@st.cache_data(show_spinner=False)
def cached_load_run(run_dir: str, reports_dir: str) -> reporting.RunArtifacts:
    return reporting.load_run(Path(run_dir), Path(reports_dir))


def main() -> None:
    args = parse_args()
    requested_reports_dir = args.reports_dir
    reports_dir = reporting.resolve_reports_dir(requested_reports_dir)

    st.set_page_config(page_title="CMF Backtest Reports", layout="wide")
    st.title("CMF Backtest Reports")

    with st.sidebar:
        st.header("Runs")
        st.caption(f"Reports directory: `{reports_dir}`")
        if reports_dir != requested_reports_dir:
            st.info(f"`{requested_reports_dir}` was not found; using sample artifacts.")

        run_dirs = cached_run_dirs(str(reports_dir))
        labels = {reporting.run_label(Path(path), reports_dir): path for path in run_dirs}
        if not labels:
            st.warning("No run directories with metrics.json were found.")
            return

        mode = st.radio("Mode", ["single run", "compare runs"], horizontal=False)
        if mode == "single run":
            selected_label = st.selectbox("Run", list(labels))
            selected_labels = [selected_label]
        else:
            default = list(labels)[: min(3, len(labels))]
            selected_labels = st.multiselect("Runs", list(labels), default=default)

        normalize_equity = st.checkbox("Normalize equity at first sample", value=False)

    if not selected_labels:
        st.warning("Select at least one run.")
        return

    runs = [cached_load_run(labels[label], str(reports_dir)) for label in selected_labels]
    start, end = reporting.combined_time_bounds(runs)

    if start is not None and end is not None and start < end:
        start_dt = start.to_pydatetime()
        end_dt = end.to_pydatetime()
        window = st.sidebar.slider("Time window", start_dt, end_dt, (start_dt, end_dt))
        runs = [
            reporting.filter_run_by_time(
                run,
                pd.Timestamp(window[0]).tz_convert("UTC"),
                pd.Timestamp(window[1]).tz_convert("UTC"),
            )
            for run in runs
        ]

    tabs = st.tabs(
        [
            "Overview",
            "Equity & PnL",
            "Inventory",
            "Quoting",
            "Fills",
            "Adverse selection",
            "Sensitivity",
        ]
    )

    with tabs[0]:
        render_overview(runs, reports_dir)
    with tabs[1]:
        render_equity(runs, normalize_equity)
    with tabs[2]:
        render_inventory(runs)
    with tabs[3]:
        render_quoting(runs)
    with tabs[4]:
        render_fills(runs)
    with tabs[5]:
        render_adverse_selection(runs)
    with tabs[6]:
        render_sensitivity(runs)


def render_overview(runs: list[reporting.RunArtifacts], reports_dir: Path) -> None:
    st.subheader("Metrics")
    metrics = reporting.metrics_table(runs)
    if metrics.empty:
        st.info("No scalar metrics are available for the selected runs.")
    else:
        st.dataframe(reporting.style_metric_table(metrics), width="stretch")

    st.subheader("Run Metadata")
    metadata = reporting.metadata_table(runs)
    st.dataframe(metadata, width="stretch")

    if len(runs) == 1:
        st.subheader("Static Export")
        if st.button("Export static report"):
            try:
                output_dir = reporting.export_static(runs[0].path, reports_dir=reports_dir)
            except Exception as exc:  # noqa: BLE001 - Streamlit should surface export failures.
                st.error(f"Static export failed: {exc}")
            else:
                st.success(f"Exported to `{output_dir}`")


def render_equity(runs: list[reporting.RunArtifacts], normalize: bool) -> None:
    st.plotly_chart(reporting.equity_figure(runs, normalize=normalize), width="stretch")
    st.plotly_chart(reporting.drawdown_figure(runs, normalize=normalize), width="stretch")


def render_inventory(runs: list[reporting.RunArtifacts]) -> None:
    left, right = st.columns(2)
    with left:
        st.plotly_chart(reporting.inventory_figure(runs), width="stretch")
    with right:
        st.plotly_chart(reporting.inventory_histogram(runs), width="stretch")


def render_quoting(runs: list[reporting.RunArtifacts]) -> None:
    left, right = st.columns(2)
    with left:
        st.plotly_chart(reporting.quote_distance_figure(runs), width="stretch")
    with right:
        st.plotly_chart(reporting.quote_spread_figure(runs), width="stretch")


def render_fills(runs: list[reporting.RunArtifacts]) -> None:
    st.plotly_chart(reporting.fills_figure(runs), width="stretch")

    for run in runs:
        with st.expander(f"Fills table: {run.name}", expanded=len(runs) == 1):
            if run.fills.empty:
                st.info("No fills were recorded.")
                continue
            page_size = st.selectbox(
                "Rows per page",
                [25, 50, 100, 250],
                index=1,
                key=f"fills_page_size_{run.name}",
            )
            pages = max(1, math.ceil(len(run.fills) / page_size))
            page = st.number_input(
                "Page",
                min_value=1,
                max_value=pages,
                value=1,
                step=1,
                key=f"fills_page_{run.name}",
            )
            st.dataframe(
                reporting.paginated_frame(run.fills, int(page), int(page_size)),
                width="stretch",
            )


def render_adverse_selection(runs: list[reporting.RunArtifacts]) -> None:
    st.plotly_chart(reporting.adverse_selection_figure(runs), width="stretch")


def render_sensitivity(runs: list[reporting.RunArtifacts]) -> None:
    frame = reporting.sensitivity_frame(runs)
    parameter_columns = [
        column
        for column in frame.columns
        if column not in {"run"} and pd.api.types.is_numeric_dtype(frame[column])
    ]
    metric_columns = [column for column in reporting.METRIC_ORDER if column in frame.columns]
    parameter_columns = [column for column in parameter_columns if column not in metric_columns]

    if len(parameter_columns) < 2 or not metric_columns:
        st.info("Sensitivity heatmap needs at least two numeric run parameters.")
        st.dataframe(frame, width="stretch")
        return

    controls = st.columns(3)
    with controls[0]:
        x_param = st.selectbox("X parameter", parameter_columns, index=0)
    with controls[1]:
        y_options = [column for column in parameter_columns if column != x_param]
        y_param = st.selectbox("Y parameter", y_options, index=0)
    with controls[2]:
        metric = st.selectbox("Metric", metric_columns, index=0)

    st.plotly_chart(
        reporting.sensitivity_heatmap(frame, x_param, y_param, metric),
        width="stretch",
    )
    st.dataframe(frame, width="stretch")


if __name__ == "__main__":
    main()
