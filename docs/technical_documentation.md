# Technical Documentation

## 1. Scope

This document describes the current CMF LOB backtester implementation. The
engine is a C++20 event-driven replay system for historical Market-By-Price
data, owned limit orders, simulated fills, portfolio accounting, metrics, and
market-making strategies.

Implemented strategies:

- no-op smoke strategy;
- fixed-spread baseline;
- classic Avellaneda-Stoikov;
- microprice-adjusted Avellaneda-Stoikov.

Python utilities are used only for data audit, experiment orchestration,
dashboarding, static export, and submission packaging.

## 2. File Map

- `lob_backtester/apps/lob_backtest.cpp` implements the CLI.
- `lob_backtester/src/lob/data/` implements the event model and streaming CSV
  data source.
- `lob_backtester/src/lob/book/` implements the Market-By-Price order book.
- `lob_backtester/src/lob/features/` implements reusable book features.
- `lob_backtester/src/lob/execution/` implements order management and fills.
- `lob_backtester/src/lob/portfolio/` implements accounting.
- `lob_backtester/src/lob/metrics/` implements run metrics and CSV/JSON output.
- `lob_backtester/src/lob/strategies/` implements strategy interfaces and
  strategy formulas.
- `lob_backtester/src/lob/engine/` implements the event loop.
- `lob_backtester/src/lob/utils/` implements config parsing and git metadata.
- `lob_backtester/tests/` contains unit and integration tests.
- `lob_backtester/configs/` contains sample strategy configs.
- `scripts/python/` contains experiment, dashboard, static export, and
  submission helpers.
- `docs/` contains the implementation plan, data audit, final report, and
  design notes.

## 3. Event Loop

The engine follows a fixed event-driven pipeline:

1. Read the next `MarketEvent`.
2. Apply the event to the public order book.
3. Record due adverse-selection markouts for prior fills.
4. Check active owned orders with `FillModel`.
5. Apply fills to `Portfolio` and `MetricsEngine`.
6. Notify the strategy through `on_fill`.
7. Invoke `on_market_event` according to `quote_refresh_ms`.
8. Send returned `OrderIntent` values to `OrderManager`.
9. Record active quote samples and mark-to-market equity.
10. Write final artifacts when replay finishes.

This ordering is intentional. A strategy callback is built from the current
book state after the event has been applied, and it never receives future
events.

## 4. Data Model

`CsvDataSource` reads:

- `lob.csv`: full 25-level book snapshots;
- `trades.csv`: trades;
- optional `depth_updates.csv`: incremental level updates if available.

The provided dataset contains snapshots and trades only. Timestamps are
microseconds since Unix epoch in UTC. The loader normalizes timestamps to
nanoseconds, prices to integer ticks, and quantities to integer lots.

The deterministic merge key is `(local_timestamp, source_priority, row_id)`.
Snapshot events are applied before trades at the same timestamp so the book is
current before fill checks.

## 5. Order Book

`OrderBook` maintains an aggregate Market-By-Price book:

- bids sorted by descending price;
- asks sorted by ascending price;
- zero-size updates remove levels;
- `max_depth` limits retained depth;
- locked/crossed books can be rejected or recovered according to config.

The book exposes best bid/ask, spread, mid, and depth-level accessors. It is
used by both features and execution checks.

## 6. Feature Engine

Reusable book features:

- `mid = (best_bid + best_ask) / 2`;
- `spread = best_ask - best_bid`;
- top-of-book imbalance;
- weighted mid;
- microprice proxy;
- rolling population standard deviation of mid returns.

These features are kept independent of strategy logic so they can be reused by
baseline strategies, A-S strategies, metrics, and tests.

## 7. Order Management

`OrderManager` owns the lifecycle of strategy orders. Supported intents:

- `SubmitLimit`;
- `Cancel`;
- `CancelAll`;
- `Replace`.

Risk gates validate:

- positive price and quantity;
- tick alignment;
- maximum inventory under worst-case same-side active fills;
- maker-only constraints when enabled.

The manager writes `orders.csv` with submitted, cancelled, rejected, replaced,
and filled lifecycle events.

## 8. Fill Model

The MVP fill model is price-cross execution:

- a buy limit fills when the selected fill reference is at or below the limit;
- a sell limit fills when the selected fill reference is at or above the limit;
- fill price is the owned order's limit price;
- partial fills are disabled by default.

Supported fill references:

- `trade_price`;
- `best_quote`;
- `mid_price`.

Maker/taker fees are configured in basis points. Negative maker bps are allowed
to represent maker rebates.

## 9. Portfolio

`Portfolio` tracks:

- cash;
- signed position;
- average entry price;
- realized PnL;
- unrealized PnL;
- mark-to-market equity;
- fees.

The accounting supports long positions, short positions, reductions, and
reversals.

## 10. Metrics

`MetricsEngine` records:

- final PnL;
- mean/max inventory;
- inventory standard deviation;
- turnover quantity and notional;
- fill count and fill rate;
- maximum drawdown;
- average quoted spread;
- average spread captured;
- quote uptime;
- adverse-selection markout at 1s and 10s horizons.

Artifacts written per run:

- `run_metadata.json`;
- `metrics.json`;
- `equity_curve.csv`;
- `inventory.csv`;
- `orders.csv`;
- `fills.csv`.

`run_metadata.json` contains the effective config hash, git commit, UTC
timestamp, config path, and applied overrides.

## 11. Configuration

YAML configs live under `lob_backtester/configs/`.

Main sections:

```yaml
run:
  symbol: MD
  input_path: data/sample
  output_dir: reports/example
  log_level: info

market:
  tick_size: 0.0000001
  lot_size: 1.0

book:
  max_depth: 50

portfolio:
  initial_cash: 0.0

execution:
  fill_model: price_cross
  fill_reference: trade_price
  partial_fills: false
  maker_bps: 0.0
  taker_bps: 0.0

strategy:
  name: noop
  gamma: 0.01
  sigma: 1.0
  k: 1.0
  horizon_seconds: 3600.0
  sigma_window_ms: 1000
  min_spread_ticks: 2
  fair_price_mode: microprice_proxy
  microprice_alpha: 1.0
  microprice_beta: 1.0
  delta_ticks: 1
  order_qty: 1
  max_inventory: 10
  quote_refresh_ms: 100
```

Plan-style aliases from the original specification are also supported:
`data.path`, `data.tick_size`, `data.lot_size`, `data.max_depth`,
`engine.quote_refresh_ms`, `fees.maker_bps`, `fees.taker_bps`, and
`portfolio.max_inventory`.

CLI overrides are applied after YAML loading:

```bash
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml \
  --override strategy.gamma=0.05 \
  --override run.output_dir=/tmp/cmf-as-gamma-005 \
  --json
```

The `config_hash` is computed from the effective config after overrides.

## 12. Strategy Models

### Fixed Spread

The baseline cancels old orders and posts:

```text
bid = floor(mid - delta_ticks)
ask = ceil(mid + delta_ticks)
```

It stops quoting a side when a possible fill would exceed `max_inventory`.
Maker-only validation is enabled by the CLI.

### Avellaneda-Stoikov

Classic A-S builds quotes around a reservation price:

```text
r_t = mid - q * gamma * sigma^2 * (T - t)
psi_t = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
bid = floor(r_t - psi_t / 2)
ask = ceil(r_t + psi_t / 2)
```

Implementation details:

- every scheduled callback emits `cancel_all` and then new maker quotes;
- `sigma_window_ms` controls rolling mid-return volatility;
- the initial `sigma` is used until at least two returns are available;
- rolling return standard deviation is converted into tick volatility by
  multiplying by the current mid;
- `horizon_seconds` starts from the first strategy callback;
- `min_spread_ticks`, maker-only checks, and `max_inventory` prevent crossed
  quotes and uncontrolled inventory.

### Microprice A-S

The microprice variant adjusts fair price using top-of-book imbalance:

```text
microprice_proxy = mid + microprice_alpha * (spread / 2) * imbalance
fair_price = mid + microprice_beta * (microprice_proxy - mid)
r_t = fair_price - q * gamma * sigma^2 * (T - t)
```

`microprice_beta = 0` is numerically equivalent to classic A-S on the same
state stream. Learned microprice is not part of the MVP and remains a roadmap
item.

## 13. Build, Test, And Format

Build:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Run the CLI:

```bash
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml
```

Format check:

```bash
find lob_backtester/apps lob_backtester/src lob_backtester/tests -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format --dry-run --Werror
```

## 14. Reporting And Experiments

Run the full experiment set:

```bash
python3 scripts/python/run_experiments.py
```

Open the dashboard:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

Use tracked sample fixtures on a clean checkout:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir data/sample_reports
```

Export a static report for one run:

```bash
python scripts/python/export_static.py --run reports/microprice_as
```

Package a submission directory:

```bash
python3 scripts/python/package_submission.py --output submission/cmf_lob_backtester
```

`run_experiments.py` runs the three base configs plus a 3x3x3
`gamma/k/beta` grid and writes `reports/experiment_summary.md` and static
sensitivity heatmaps. `package_submission.py` copies README, configs, source,
tests, sample data, report docs, base run artifacts, and charts into an ignored
submission directory.

## 15. Test Strategy

Coverage areas:

- config parsing and overrides;
- CSV parsing and timestamp-order validation;
- order-book snapshot/update behavior and crossed-book recovery;
- feature formulas and rolling volatility;
- order lifecycle and risk gates;
- fill model behavior, fee validation, and maker rebates;
- portfolio accounting;
- metrics aggregation and atomic validation;
- strategy formulas and inventory guards;
- engine integration, no look-ahead behavior, sample replay throughput, and
  adverse-selection markout.

Synthetic tests cover deterministic edge cases. The committed one-hour sample
is used for integration and throughput checks.

## 16. Final Deliverables

- C++ backtest engine and strategy source.
- YAML configs for fixed spread, classic A-S, and microprice A-S.
- Unit and integration tests.
- Committed sample dataset.
- Experiment runner and dashboard.
- Final report and technical documentation.
- Reproducible local submission package.
