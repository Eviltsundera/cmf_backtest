# AGENTS.md

## Repository Context

This repository contains the CMF LOB backtester for the HFT entrance task. The
target implementation is an event-driven C++20 backtester for historical
limit-order-book replay, Avellaneda-Stoikov market making, and a microprice
extension.

Primary planning source:

- `docs/implementation_plan.md`

Read that file before selecting the next task. When a task or DoD item is
finished, update the plan with the actual status.

## Project Layout

- `lob_backtester/` - C++ backtester project.
- `lob_backtester/src/lob/` - core domain modules.
- `lob_backtester/apps/lob_backtest.cpp` - CLI entry point.
- `lob_backtester/configs/` - YAML run configs.
- `lob_backtester/tests/` - GoogleTest tests.
- `scripts/python/` - experiment runner, dashboard, static report export.
- `lob_backtester/scripts/python/` - raw data audit helper.
- `docs/` - task description, implementation plan, design notes, report, and
  technical documentation.
- `data/` - committed sample data plus ignored local raw market data. Do not
  commit raw datasets or extracted archives.

## Build And Verify

Run commands from the repository root:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lob_backtest --config lob_backtester/configs/example.yaml
```

Format check:

```bash
find lob_backtester/apps lob_backtester/src lob_backtester/tests -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format --dry-run --Werror
```

If dependencies are not downloaded yet, CMake fetches `yaml-cpp`, `spdlog`, and
`googletest` through `FetchContent`.

## Development Rules

- Preserve the current C++20/CMake structure.
- Keep small domain modules under
  `src/lob/{data,book,features,execution,portfolio,strategies,engine,metrics,utils}`.
- Keep the engine event-driven: market event, book update, fills, strategy,
  order management, portfolio, metrics.
- Avoid look-ahead bias: strategies may observe only state produced by already
  applied events.
- Add focused tests together with code, especially for parsing, book
  invariants, fills, accounting, risk gates, and strategy formulas.
- Do not commit raw data, build output, generated reports, generated
  submission packages, or local virtual environments unless a task explicitly
  requires otherwise.

## Current State

Tasks T0 through T15 are complete. The repository currently includes:

- CMake project with `lob_core`, `lob_backtest`, and `lob_tests`.
- YAML config loader in `lob::utils` with plan-style aliases, repeated
  `--override` support, and stable effective config hashing.
- Streaming CSV DataLoader in `lob::data`.
- Market-By-Price `OrderBook` in `lob::book`.
- Stateless order-book features and rolling volatility in `lob::features`.
- OrderManager lifecycle/risk gates and FillModel in `lob::execution`.
- Portfolio accounting in `lob::portfolio`.
- MetricsEngine output for `metrics.json`, `equity_curve.csv`, and
  `inventory.csv`.
- Strategy interface, `NoopStrategy`, `FixedSpreadStrategy`, and
  `AvellanedaStoikovStrategy` with classic and microprice fair-price modes.
- `BacktestEngine` event loop with fills, portfolio updates, quote samples,
  equity samples, and 1s/10s adverse-selection markouts.
- CLI replay path:
  `lob_backtest --config <yaml> [--override key=value ...] [--json]`.
- Baseline configs for fixed spread, classic A-S, and microprice A-S.
- Run artifacts: `run_metadata.json`, `metrics.json`, `equity_curve.csv`,
  `inventory.csv`, `orders.csv`, and `fills.csv`.
- Python reporting stack: experiment runner, Streamlit dashboard, static export,
  and submission packager.
- English documentation and final report under `docs/`.

Next planned work is optional roadmap work, not required for the MVP
submission.
