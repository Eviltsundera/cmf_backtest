# CMF LOB Backtester

Event-driven C++20 backtest engine for historical limit-order-book replay and
market-making strategies. The target scope is a reproducible CMF HFT submission
with data audit, a limit-order-book simulator, Avellaneda-Stoikov strategies,
metrics, reports, and documentation.

## Current Status

Implemented:

- CMake bootstrap for `lob_core`, `lob_backtest`, and `lob_tests`.
- YAML config loader.
- Streaming CSV DataLoader for `lob.csv`, `trades.csv`, and optional
  `depth_updates.csv`.
- Normalized `MarketEvent` stream ordered by `(ts_ns, seq)`.
- Market-By-Price `OrderBook` with snapshot/update replay and crossed-book
  recovery policies.
- Order-book feature functions for mid, spread, imbalance, weighted mid,
  microprice proxy, and rolling mid-return volatility.
- OrderManager lifecycle for limit order submit/cancel/replace/fill, risk gates,
  and `orders.csv` logging.
- FillModel for trade-price, best-quote, and mid-price execution checks with
  maker/taker fee accounting.
- Portfolio accounting for cash, signed inventory, realized/unrealized PnL, and
  mark-to-market equity.
- MetricsEngine for PnL, inventory, turnover, fill-rate, drawdown,
  market-making metrics, and `metrics.json`/CSV outputs.
- BacktestEngine event loop integrating DataLoader, OrderBook, features,
  strategy callbacks, OMS, FillModel, Portfolio, MetricsEngine, and run
  artifacts including `orders.csv` and `fills.csv`.
- Fixed-spread baseline strategy with YAML config, inventory guard, maker-only
  risk validation, synthetic PnL test, and sample artifact smoke coverage.
- Avellaneda-Stoikov strategy with formula tests, rolling mid-return
  volatility, inventory guard, maker-only validation, YAML config, and sample
  artifact smoke coverage.
- Microprice-adjusted Avellaneda-Stoikov mode with configurable
  `fair_price_mode`, `microprice_alpha`, `microprice_beta`, formula tests,
  beta-zero equivalence coverage, YAML config, and sample artifact smoke
  coverage.
- CLI/config polish with repeated `--override key=value`, `--json` summary
  output, config-driven log level, plan-style YAML aliases, stable effective
  config hash, and `run_metadata.json`.
- Reporting layer with a Streamlit dashboard, multi-run comparison, sensitivity
  heatmaps, and static Markdown/PNG export for submission artifacts.
- Data audit and deterministic one-hour sample under `data/sample/`.
- GoogleTest coverage for config loading, event parsing, timestamp validation,
  k-way merge, sample replay, order-book invariants, feature formulas, OMS
  lifecycle/fill rules, portfolio accounting, run metrics, engine integration,
  look-ahead protection, fixed-spread, Avellaneda-Stoikov, microprice A-S
  strategy behavior, and sample replay throughput.

Next planned task: T14, experiments and final report.

## Repository Layout

```text
.
|-- AGENTS.md                    # agent-facing project context
|-- data/
|   |-- sample/                  # committed deterministic test sample
|   `-- raw/                     # local raw data, ignored by git
|-- docs/
|   |-- implementation_plan.md   # task tracker and DoD
|   |-- data_audit.md            # raw data schema and sample audit
|   `-- technical_documentation.md
|-- scripts/python/              # dashboard and static report export
`-- lob_backtester/
    |-- apps/                    # CLI entry points
    |-- configs/                 # YAML configs
    |-- scripts/python/          # data audit and future reports
    |-- src/lob/                 # C++ engine modules
    `-- tests/                   # GoogleTest tests
```

## Build

Run from the repository root:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

CMake fetches third-party dependencies through `FetchContent`:

- `yaml-cpp`
- `spdlog`
- `googletest`

## Test

```bash
ctest --test-dir build --output-on-failure
```

If `ctest` is not available in the shell, run the test binary directly:

```bash
./build/lob_tests
```

The current sample integration test drains `757,667` events from `data/sample`
and records loader and engine throughput.

## Run

```bash
./build/lob_backtest --config lob_backtester/configs/example.yaml
```

The CLI runs the configured CSV replay and writes run artifacts to
`run.output_dir`, including `run_metadata.json` with the effective config hash,
git commit, timestamp, config path, and CLI overrides. Strategy configs are
available via:

```bash
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml
```

Runtime overrides are applied after YAML loading:

```bash
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml \
  --override strategy.gamma=0.05 \
  --override run.output_dir=/tmp/cmf-as-gamma-005 \
  --json
```

## Viewing Results

Install the Python reporting dependencies from the repository root:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Run the dashboard against generated reports:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

For tracked sample report fixtures that work on a clean checkout, use:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir data/sample_reports
```

![Dashboard overview](docs/assets/dashboard_overview.svg)

Export a static submission report without Streamlit:

```bash
python scripts/python/export_static.py --run data/sample_reports/avellaneda_stoikov
```

Generate fresh sample run artifacts into ignored `reports/` directories:

```bash
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml \
  --override run.output_dir=reports/baseline_fixed
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml \
  --override run.output_dir=reports/avellaneda_stoikov
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml \
  --override run.output_dir=reports/microprice_as
```

## Data

Tracked data:

- `data/sample/lob.csv`
- `data/sample/trades.csv`
- `data/sample/manifest.json`
- `data/sample/audit_summary.json`

Ignored local data:

- `data/raw/`
- `*.zip`

Regenerate the data audit and sample summary:

```bash
python3 lob_backtester/scripts/python/audit.py --json-out data/sample/audit_summary.json
```

## Documentation

- [Implementation plan](docs/implementation_plan.md)
- [Technical documentation](docs/technical_documentation.md)
- [Data audit](docs/data_audit.md)
- [Backtester README](lob_backtester/README.md)
- [Agent context](AGENTS.md)

## Development Notes

- Keep the engine event-driven: market event, book update, fills, strategy,
  order management, portfolio, metrics.
- Avoid look-ahead bias: strategies may only observe state produced by already
  applied events.
- Do not commit raw datasets, build output, generated reports, or local virtual
  environments.
