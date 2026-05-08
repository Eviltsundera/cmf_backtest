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
- Data audit and deterministic one-hour sample under `data/sample/`.
- GoogleTest coverage for config loading, event parsing, timestamp validation,
  k-way merge, and sample replay.

Next planned task: T3, Market-By-Price `LOBBuilder` / `OrderBook`.

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
and records loader throughput in `docs/data_audit.md`.

## Run

```bash
./build/lob_backtest --config lob_backtester/configs/example.yaml
```

The current CLI is still a smoke path: it loads the YAML config, logs the path,
prints parsed parameters, and exits.

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
