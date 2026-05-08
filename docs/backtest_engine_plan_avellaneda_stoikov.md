# Backtest Engine Design Plan: Avellaneda-Stoikov And Microprice

## 1. Objective

Build an event-driven backtest engine that replays historical limit-order-book
data, manages owned limit orders, simulates fills, computes trading metrics, and
evaluates market-making strategies.

Required strategies:

- fixed-spread baseline;
- Avellaneda-Stoikov (2008);
- microprice-adjusted Avellaneda-Stoikov inspired by Stoikov (2018).

## 2. MVP Scope

The MVP focuses on deterministic replay and reproducibility:

- Market-By-Price order book reconstruction.
- Owned limit order submit/cancel/replace/fill lifecycle.
- Price-cross execution model.
- Portfolio accounting and metrics.
- Experiment runner and dashboard.
- Final report and submission package.

Out of scope for the MVP:

- order-level queue reconstruction;
- partial fills;
- latency;
- market impact;
- learned microprice.

These remain roadmap items.

## 3. Data Assumptions

The provided `MD.zip` archive contains:

- `lob.csv`: full 25-level order book snapshots;
- `trades.csv`: trades.

No native incremental depth-update stream is present. Timestamps are Unix
microseconds in UTC. The engine normalizes them to nanoseconds and represents
prices/quantities as integer ticks/lots.

Recommended merge key:

```text
(local_timestamp, source_priority, row_id)
```

For equal timestamps, apply book snapshots before trades.

## 4. Event-Driven Architecture

The replay loop must preserve this order:

1. Read one market event.
2. Apply it to the public order book.
3. Check due adverse-selection markouts from previous fills.
4. Fill active owned orders when the current market event crosses their levels.
5. Apply fills to portfolio and metrics.
6. Notify the strategy about fills.
7. Call the strategy on the current market state.
8. Process strategy order intents through the OMS.
9. Record quote and equity samples.

This order avoids look-ahead bias: strategies observe only current or past
state.

## 5. Module Responsibilities

### DataLoader

- Stream CSV without loading the full dataset into memory.
- Normalize timestamps, prices, and quantities.
- Validate timestamp monotonicity within each source.
- Merge multiple sources deterministically.

### OrderBook

- Apply snapshots and optional updates.
- Maintain bids in descending price order and asks in ascending price order.
- Detect and handle locked/crossed books.
- Expose best bid/ask, mid, spread, and depth.

### FeatureEngine

- Compute mid, spread, top-of-book imbalance, weighted mid, microprice proxy,
  and rolling volatility.

### OrderManager

- Own strategy orders.
- Support submit, cancel, cancel-all, replace, and fill transitions.
- Enforce inventory, tick, quantity, and maker-only risk gates.
- Write order lifecycle artifacts.

### FillModel

- Simulate price-cross fills.
- Support trade-price, best-quote, and mid-price references.
- Apply maker/taker fees and maker rebates.

### Portfolio

- Track cash, signed inventory, average entry price, realized PnL, unrealized
  PnL, and equity.

### MetricsEngine

- Aggregate PnL, inventory, turnover, fill rate, drawdown, spread capture,
  quote uptime, and adverse-selection markouts.

### BacktestEngine

- Compose all modules in the fixed event-loop order.
- Write run artifacts.

## 6. Fixed-Spread Baseline

The baseline posts symmetric quotes around mid:

```text
bid = mid - delta_ticks
ask = mid + delta_ticks
```

It cancels old quotes before refreshing and stops quoting a side if a fill would
breach the configured inventory limit.

## 7. Avellaneda-Stoikov Model

Classic A-S uses exponential utility and inventory-aware reservation price:

```text
r_t = S_t - q_t * gamma * sigma^2 * (T - t)
```

The quoted spread approximation:

```text
psi_t = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
```

Quotes:

```text
bid = r_t - psi_t / 2
ask = r_t + psi_t / 2
```

Interpretation:

- higher `gamma` increases risk aversion;
- higher `sigma` widens quotes;
- larger positive inventory lowers the reservation price;
- larger negative inventory raises the reservation price;
- `k` controls fill intensity sensitivity.

## 8. Microprice Extension

Top-of-book imbalance:

```text
imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty)
```

Proxy microprice:

```text
microprice_proxy = mid + alpha * (spread / 2) * imbalance
```

Fair price adjustment:

```text
fair_price = mid + beta * (microprice_proxy - mid)
```

Microprice A-S reservation price:

```text
r_t = fair_price - q_t * gamma * sigma^2 * (T - t)
```

`beta = 0` is equivalent to classic A-S. `beta = 1` fully uses the microprice
proxy. Intermediate `beta` values are useful because top-of-book imbalance is a
noisy signal.

## 9. Configuration

Each run is configured through YAML:

```yaml
run:
  symbol: MD
  input_path: data/sample
  output_dir: reports/microprice_as

market:
  tick_size: 0.0000001
  lot_size: 1.0

execution:
  fill_model: price_cross
  fill_reference: trade_price
  maker_bps: 0.0
  taker_bps: 0.0

strategy:
  name: microprice_as
  gamma: 0.01
  sigma: 1.0
  k: 1.0
  horizon_seconds: 3600.0
  microprice_alpha: 1.0
  microprice_beta: 1.0
  order_qty: 1
  max_inventory: 10
```

The CLI supports repeated overrides:

```bash
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml \
  --override strategy.gamma=0.02 \
  --override strategy.microprice_beta=0.5 \
  --override run.output_dir=reports/grid/run_001
```

## 10. Metrics And Artifacts

Per-run artifacts:

- `run_metadata.json`;
- `metrics.json`;
- `equity_curve.csv`;
- `inventory.csv`;
- `orders.csv`;
- `fills.csv`.

Core metrics:

- final PnL;
- maximum drawdown;
- mean/max inventory;
- inventory standard deviation;
- turnover quantity/notional;
- fill count;
- fill rate;
- average quoted spread;
- average spread captured;
- adverse-selection markout at configured horizons;
- quote uptime.

## 11. Experiments

Required base comparisons:

- fixed-spread baseline;
- classic A-S;
- microprice A-S.

Sensitivity grid:

- `gamma`: risk aversion;
- `k`: fill intensity sensitivity;
- `beta`: microprice signal strength.

The committed runner uses a 3x3x3 grid:

```text
gamma in {0.005, 0.01, 0.02}
k     in {0.5, 1.0, 2.0}
beta  in {0.0, 0.5, 1.0}
```

## 12. Reporting

The dashboard should support:

- overview metric table;
- equity and drawdown charts;
- inventory time series and distribution;
- quote diagnostics;
- fill diagnostics;
- adverse-selection bars;
- sensitivity heatmaps.

Static report export is required so the submission can be reviewed without
running Streamlit.

## 13. Testing Strategy

Minimum test coverage:

- config loading and overrides;
- CSV parsing and timestamp validation;
- k-way event merge;
- book snapshot/update behavior;
- crossed-book recovery;
- feature formulas;
- order lifecycle and risk gates;
- fill model behavior;
- portfolio accounting;
- metrics aggregation;
- strategy formulas;
- engine integration and no-look-ahead behavior;
- sample replay throughput.

Synthetic tests should cover deterministic edge cases. The committed sample
data should be used for integration and throughput checks.

## 14. Improvement Roadmap

### 14.1. Post-MVP

1. **Partial fills**: fill by available volume or trade size.
2. **Queue position model**: estimate queue position from Market-By-Price data.
3. **Latency model**: submit latency, cancel latency, jitter, and exchange ack
   delay.
4. **Market impact**: at least a temporary impact model for larger orders.
5. **Fees/rebates**: tiered fee schedules and venue-specific rebates.
6. **Multi-level microprice**: imbalance across several book levels.
7. **Learned microprice**: transition matrix or ML one-tick-ahead model.
8. **Walk-forward optimization**: train/validation/test workflow.
9. **PnL decomposition**: spread capture, inventory drift, adverse selection,
   and fees.
10. **Performance optimization**: profile-driven improvements to parser,
    replay, and metrics.

### 14.2. More Advanced Models

- Guéant-Lehalle-Fernandez-Tapia style market making.
- Skew from an external alpha signal.
- Multi-asset or cross-venue inventory constraints.
- Queue-reactive quoting.
- Learned fill probability model.

## 15. Submission Checklist

- README with build/run instructions.
- Configs for all three strategies.
- Source code for engine and strategies.
- Unit and integration tests.
- Committed sample data.
- Final report.
- Base run artifacts: orders, fills, equity curve, inventory, and metrics.
- Charts.
- Explicit execution model limitations.
- Roadmap.
- Clean-copy smoke test.
