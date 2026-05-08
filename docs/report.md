# CMF LOB Backtester Final Report

## 1. Problem Statement

The project goal is a reproducible event-driven backtest engine for
market-making strategies on historical Market-By-Price order book data. The MVP
compares three approaches:

- fixed-spread baseline;
- Avellaneda-Stoikov strategy;
- microprice-adjusted Avellaneda-Stoikov extension.

The primary evaluation metrics are net PnL, drawdown, inventory risk, turnover,
fill rate, spread capture, and adverse-selection markout.

## 2. Engine Architecture

The architecture is described in [implementation_plan.md](implementation_plan.md)
and [technical_documentation.md](technical_documentation.md). The execution flow
is:

1. `CsvDataSource` streams `lob.csv` and `trades.csv` into a single ordered
   event stream.
2. `OrderBook` applies snapshots/updates and maintains best bid/ask.
3. `FeatureEngine` computes mid, spread, imbalance, weighted mid, and the
   microprice proxy.
4. A strategy emits `OrderIntent` values.
5. `OrderManager` validates risk gates and tracks the lifecycle of owned
   orders.
6. `FillModel` fills active limit orders using the price-cross rule.
7. `Portfolio` and `MetricsEngine` write `metrics.json`, `equity_curve.csv`,
   `inventory.csv`, `orders.csv`, and `fills.csv`.

The engine avoids look-ahead bias: on each event the strategy sees only book,
portfolio, and active-order state that has already been applied.

## 3. Models

The fixed-spread baseline posts symmetric limit orders:

```text
bid = mid - delta_ticks
ask = mid + delta_ticks
```

Avellaneda-Stoikov uses a reservation price and optimal spread:

```text
r_t = mid - q * gamma * sigma^2 * (T - t)
psi_t = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
bid = r_t - psi_t / 2
ask = r_t + psi_t / 2
```

The microprice extension adjusts the fair price:

```text
microprice_proxy = mid + alpha * (spread / 2) * imbalance
fair_price = mid + beta * (microprice_proxy - mid)
r_t = fair_price - q * gamma * sigma^2 * (T - t)
```

`beta = 0` matches classic A-S. `beta = 1` uses the full proxy adjustment.

## 4. Experiment Methodology

Experiments use the committed sample dataset under `data/sample`:

- interval: `2024-08-05T06:00:00Z` to `2024-08-05T07:00:00Z`;
- snapshots: 7,200;
- trades: 750,467;
- total replay events: 757,667.

Base runs:

- `baseline_fixed`;
- `avellaneda_stoikov`;
- `microprice_as`.

Sensitivity grid: `gamma in {0.005, 0.01, 0.02}`,
`k in {0.5, 1.0, 2.0}`, and `beta in {0.0, 0.5, 1.0}`. This gives 27
microprice-A-S runs. `beta=0` acts as the classic A-S baseline inside the grid.
Walk-forward splitting was not used because the committed dataset is a one-hour
sample; a larger full/raw dataset remains a local untracked input.

## 5. Results

The metrics below were produced with `python3 scripts/python/run_experiments.py`.
PnL, drawdown, and markout are reported in engine tick/notional units.

| Strategy | Net PnL | Max DD | Mean inv | Max inv | Turnover qty | Fill rate | Spread captured | Adv 1s | Adv 10s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed spread | -741593 | 742064 | 0.601 | 10 | 31618 | 0.598724 | 1.001 | -24.153 | -23.945 |
| A-S | -4345 | 48629.5 | 6.078 | 10 | 30 | 0.001700 | 9.617 | -7.117 | -39.063 |
| Microprice A-S | -4345 | 48633.5 | 6.071 | 10 | 28 | 0.001559 | 8.304 | -4.321 | -25.750 |

On the base config, A-S and Microprice-A-S have identical net PnL, but
Microprice-A-S gets fewer fills and better 1s/10s adverse-selection markout.
Fixed spread produces high turnover and fill rate but loses heavily on PnL and
drawdown.

Best grid run: `gamma=0.02, k=1.0, beta=1.0`, net PnL `77`.
Average grid PnL by `beta`:

| beta | Mean grid PnL |
| ---: | ---: |
| 0.0 | -6072.389 |
| 0.5 | -4213.944 |
| 1.0 | -4383.889 |

On average across the grid, microprice-aware runs (`beta > 0`) are not worse
than the classic A-S proxy (`beta = 0`) on net PnL for this sample.

## 6. Charts

The dashboard plots overlay equity curves, drawdown, inventory time series,
inventory distributions, fill diagnostics, adverse-selection bars, and
sensitivity heatmaps:

```bash
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

Static heatmaps from the latest grid are committed as tracked assets:

![Final PnL by gamma and beta](assets/final_pnl_gamma_beta.svg)

![Final PnL by gamma and k](assets/final_pnl_gamma_k.svg)

Generated local artifacts after running the experiment runner:

- `reports/experiment_summary.md`;
- `reports/_static/sensitivity/final_pnl_gamma_beta.svg`;
- `reports/_static/sensitivity/final_pnl_gamma_k.svg`;
- `reports/grid_gamma_k_beta/*/{metrics.json,equity_curve.csv,inventory.csv,orders.csv,fills.csv}`.

## 7. Discussion

Fixed spread is too aggressive for this sample: it keeps a high fill rate but
accumulates adverse selection and drawdown quickly.

A-S sharply reduces turnover and drawdown through wider quotes and inventory
skew. The cost is a low fill rate and few trades.

The microprice extension is useful as a directional fair-price correction. On
the base config, net PnL matched A-S while markout improved. In the grid, the
best result came from higher `gamma=0.02`, `k=1.0`, `beta=1.0`. Average PnL was
stronger at `beta=0.5` than at `beta=1.0`, suggesting that fully trusting a
noisy top-of-book imbalance proxy may over-skew quotes.

## 8. Execution Model Limitations

- Execution uses a price-cross model; real queue position is not modeled.
- Partial fills are disabled: an order either fills completely or not at all.
- There is no submit/cancel latency, exchange acknowledgement delay, or jitter.
- There is no market impact: owned trades do not alter future book state.
- Market-By-Price data does not allow order-level queue reconstruction.
- Fees/rebates are supported in config, but the main experiments use zero fees.
- Volatility is estimated online from rolling mid returns; there is no separate
  train split calibration for `sigma`.
- Adverse-selection markout is recorded on the first available event after the
  1s/10s horizon; unresolved fills near the end of the sample are excluded from
  the average.

## 9. Improvement Roadmap

The roadmap is based on
[backtest_engine_plan_avellaneda_stoikov.md §14](backtest_engine_plan_avellaneda_stoikov.md#14-improvement-roadmap):

1. Partial fills by trade size or available volume.
2. Queue position model on top of Market-By-Price data.
3. Submit/cancel latency model.
4. Market impact model for larger orders.
5. Tiered fees/rebates.
6. Multi-level microprice.
7. Learned microprice through a transition matrix or ML one-tick-ahead model.
8. Walk-forward optimization.
9. PnL decomposition: spread capture, inventory drift, adverse selection, fees.
10. Performance optimization.

## 10. Reproduction

Build and test:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run experiments:

```bash
python3 scripts/python/run_experiments.py
```

Run against a local full/raw dataset:

```bash
python3 scripts/python/run_experiments.py --input-path data/raw/MD
```

View results:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
streamlit run scripts/python/dashboard.py -- --reports-dir reports/
```

Export one static run report:

```bash
python scripts/python/export_static.py --run reports/microprice_as
```
