# Experiments Runbook

This document records the reproducible experiment set used by
[report.md](report.md). Generated run artifacts are intentionally ignored by
git and live under `reports/`.

## Dataset

Default tracked input:

- `data/sample/lob.csv`
- `data/sample/trades.csv`
- interval: `2024-08-05T06:00:00Z` .. `2024-08-05T07:00:00Z`
- replay events: 757,667

To run against a larger local dataset, pass `--input-path` to the runner.

## Commands

Build first:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run all T14 experiments:

```bash
python3 scripts/python/run_experiments.py
```

Run on a local full dataset:

```bash
python3 scripts/python/run_experiments.py --input-path data/raw/MD
```

Open the dashboard:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

## Experiment Set

Base runs:

- `reports/baseline_fixed`
- `reports/avellaneda_stoikov`
- `reports/microprice_as`

Grid runs:

- root: `reports/grid_gamma_k_beta/`
- `gamma`: `0.005`, `0.01`, `0.02`
- `k`: `0.5`, `1.0`, `2.0`
- `microprice_beta`: `0.0`, `0.5`, `1.0`

Runner outputs:

- `reports/experiment_manifest.json`
- `reports/experiment_summary.md`
- `reports/_static/sensitivity/final_pnl_gamma_beta.svg`
- `reports/_static/sensitivity/final_pnl_gamma_k.svg`
- `reports/_static/sensitivity/grid_metrics.csv`

The latest committed report also stores the two final-PnL heatmaps in
`docs/assets/` so [report.md](report.md) renders without ignored local files.

## Latest Result Summary

| Strategy | Net PnL | Max DD | Turnover qty | Fill rate | Adv 1s | Adv 10s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed spread | -759847 | 760318 | 29016 | 0.552349 | -27.261 | -26.572 |
| A-S | -4346 | 48629.5 | 30 | 0.001700 | -7.150 | -38.854 |
| Microprice A-S | -4362 | 48633.5 | 24 | 0.001376 | -11.688 | -34.792 |

Best grid run: `gamma=0.02`, `k=1.0`, `beta=1.0`, net PnL `45`.
