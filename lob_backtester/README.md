# LOB Backtester

Event-driven C++20 backtest engine for historical limit-order-book replay and
market-making strategies.

## Build

Dependencies are fetched by CMake through `FetchContent`:

- `yaml-cpp`
- `spdlog`
- `googletest`

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

If `ctest` is not available in the shell, the same test binary can be run
directly:

```bash
./build/lob_tests
```

## Run

```bash
./build/lob_backtest --config lob_backtester/configs/example.yaml
```

The CLI runs the configured CSV replay, prints a short run summary, and writes
artifacts to `run.output_dir`. Each run writes `run_metadata.json` with the
effective config hash, git commit, timestamp, config path, and applied
overrides. Strategy configs are:

```bash
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml
```

Use repeated `--override key=value` flags to change the effective config after
YAML loading, and `--json` for machine-readable summary output:

```bash
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml \
  --override strategy.gamma=0.05 \
  --override run.output_dir=/tmp/cmf-as-gamma-005 \
  --json
```

Implemented engine modules currently include DataLoader, Market-By-Price
OrderBook, FeatureEngine, Strategy interface, OrderManager, FillModel,
Portfolio, MetricsEngine, BacktestEngine, NoopStrategy, FixedSpreadStrategy,
AvellanedaStoikovStrategy, and the microprice-adjusted A-S fair-price mode.

## Viewing Results

Python reporting utilities are available from the repository root under
`scripts/python`.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
streamlit run scripts/python/dashboard.py -- --reports-dir data/sample_reports
```

Static Markdown/PNG export for one run:

```bash
python scripts/python/export_static.py --run data/sample_reports/avellaneda_stoikov
```

Reproduce the full experiment set from the repository root:

```bash
python3 scripts/python/run_experiments.py
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

To run the same configs against a local full dataset, pass an input override:

```bash
python3 scripts/python/run_experiments.py --input-path data/raw/MD
```

## Documentation

- [Implementation plan](../docs/implementation_plan.md)
- [Technical documentation](../docs/technical_documentation.md)
- [Experiments runbook](../docs/experiments.md)
- [Final report](../docs/report.md)

## Format And Lint

```bash
find lob_backtester/apps lob_backtester/src lob_backtester/tests -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format --dry-run --Werror
clang-tidy -p build lob_backtester/apps/lob_backtest.cpp lob_backtester/src/lob/utils/Config.cpp
```

## Python Utilities

Python scripts for data audit, plotting, and report generation live under
`lob_backtester/scripts/python`.

```bash
python3.11 -m venv lob_backtester/scripts/python/.venv
source lob_backtester/scripts/python/.venv/bin/activate
pip install -r lob_backtester/scripts/python/requirements.txt
```

Run the raw data audit and regenerate the deterministic one-hour sample:

```bash
python3 lob_backtester/scripts/python/audit.py --json-out data/sample/audit_summary.json
```
