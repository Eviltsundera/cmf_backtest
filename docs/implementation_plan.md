# Implementation Plan: LOB Backtest Engine + Avellaneda-Stoikov / Microprice

**Version:** 1.0
**Goal:** deliver the CMF HFT entrance project: an event-driven C++ backtest
engine for historical limit-order-book data, Avellaneda-Stoikov strategies,
experiments, report, and documentation.

## Stack

- **Core:** C++20, CMake 3.20+, GoogleTest, clang-format, clang-tidy.
- **Dependencies:** `yaml-cpp`, `spdlog`, GoogleTest through CMake
  `FetchContent`.
- **Post-processing:** Python 3.11+, pandas, Plotly, Streamlit.
- **Project root:** `lob_backtester/`.
- **Data:** committed sample under `data/sample/`; raw local data under ignored
  `data/raw/`.

## MVP Assumptions

- Market-By-Price aggregate order book.
- No order-level queue reconstruction.
- Price-cross fill model.
- Fill price equals the owned order limit price.
- Partial fills are disabled in the MVP.
- Latency is zero.
- Single symbol per run.
- Generated reports and submission packages are local artifacts, not committed.

## Dependency Map

```text
T0 ─┬─ T1 ── T2 ── T3 ── T4 ── T7 ── T8 ─┬─ T9  ── T11 ── T12 ── T13 ── T14 ── T15
    │                  └─ T5 ── T6 ──────┘   └─ T10 ─┘
    └─ data audit and bootstrap come first; all other tasks depend on them.
```

Critical path: T0 -> T1 -> T2 -> T3 -> T7 -> T8 -> T10 -> T11 -> T13 -> T14
-> T15.

## T0. Repository Bootstrap [done]

**Goal:** initialize the C++ project, build system, tests, format/lint config,
and CLI skeleton.

**Implemented:**

- `lob_backtester/` CMake project.
- `lob_core`, `lob_backtest`, and `lob_tests` targets.
- `.clang-format` and `.clang-tidy`.
- YAML config loader.
- CLI smoke path.
- Root and backtester READMEs.

**DoD:** build, tests, example CLI run, formatting check, and README commands
were verified.

## T1. Data Audit And Schema Definition [done]

**Goal:** understand `MD.zip`, document the schema, and commit a small
deterministic sample.

**Implemented:**

- `docs/data_audit.md` with file schemas, ranges, event counts, sample interval,
  and preprocessing decision.
- `data/sample/` with one-hour `lob.csv`, `trades.csv`, `manifest.json`, and
  `audit_summary.json`.
- `lob_backtester/scripts/python/audit.py`.

**Decision:** parse CSV directly in the C++ engine for the MVP. A binary
preprocessing stage is a future optimization only if CSV replay becomes a
bottleneck.

## T2. DataLoader: Event Stream Parsing And Normalization [done]

**Goal:** stream raw CSV files into normalized `MarketEvent` values.

**Implemented:**

- `MarketEvent`, payload structs, and event counts.
- `CsvDataSource` for snapshots, trades, and optional depth updates.
- Timestamp normalization from microseconds to nanoseconds.
- Price and quantity normalization to integer ticks/lots.
- Deterministic k-way merge by timestamp and sequence.
- Parser validation for timestamp regressions, duplicate keys, and tick
  misalignment.

**DoD:** parser tests, timestamp-order tests, k-way merge tests, sample replay,
and throughput measurement passed.

## T3. LOBBuilder: Market-By-Price Order Book [done]

**Goal:** reconstruct the public order book from snapshots and updates.

**Implemented:**

- `OrderBook` with bid/ask maps.
- Snapshot application and zero-size level removal.
- Configurable depth cap.
- Crossed-book reject/recover policies.
- Best bid/ask, spread, mid, and level accessors.

**DoD:** snapshot/update tests, crossed-book tests, negative snapshot quantity
rejection, property-style random update test, and update benchmark passed.

## T4. FeatureEngine: Order Book Features [done]

**Goal:** compute reusable features for strategies and metrics.

**Implemented:**

- mid price;
- spread;
- top-of-book imbalance;
- weighted mid;
- microprice proxy;
- rolling mid-return population standard deviation.

**DoD:** formula and edge-case tests passed, including empty side behavior,
balanced-volume microprice, alpha-zero microprice, and rolling std comparison.

## T5. OMS: Owned Order Lifecycle [done]

**Goal:** track strategy-owned limit orders.

**Implemented:**

- `OrderIntent` and `OrderStatus`.
- submit, cancel, cancel-all, replace, fill lifecycle.
- active order store and event log.
- `orders.csv` output.
- risk gates for max inventory, min quantity, tick alignment, and maker-only
  checks.

**DoD:** lifecycle tests, risk-gate tests, replace tests, maker-only tests, and
CSV lifecycle logging passed.

## T6. FillModel: Execution Simulator [done]

**Goal:** simulate owned order fills against historical market events.

**Implemented:**

- price-cross fill model.
- fill references: trade price, best quote, and mid price.
- limit-price fill output.
- maker/taker fee calculation.
- maker rebates through negative maker bps.

**DoD:** buy/sell fill tests, reference fallback tests, fee tests, maker rebate
test, and synthetic round-trip PnL test passed.

## T7. Portfolio + MetricsEngine [done]

**Goal:** account for fills and produce run-level metrics.

**Implemented:**

- `Portfolio` cash, signed inventory, average entry price, realized PnL,
  unrealized PnL, fees, and equity.
- `MetricsEngine` final PnL, inventory statistics, turnover, fill rate,
  drawdown, quoted spread, spread captured, quote uptime, and
  adverse-selection markout.
- `metrics.json`, `equity_curve.csv`, and `inventory.csv`.

**DoD:** accounting tests, no-trading sanity, zero-fee spread sanity,
metrics output tests, and validation atomicity tests passed.

## T8. Engine Event Loop And Integration [done]

**Goal:** connect DataLoader, OrderBook, strategy callbacks, OMS, FillModel,
Portfolio, and MetricsEngine.

**Implemented:**

- `BacktestEngine` event loop.
- deterministic callback order.
- fill batch accounting before fill callbacks.
- fill-opportunity accounting for submits and replaces.
- active quote sampling.
- output artifacts.
- 1s and 10s adverse-selection markout scheduling.

**DoD:** synthetic integration, no-look-ahead test, sample replay throughput,
fill batch accounting tests, replace fill-rate test, and adverse-selection
horizon tests passed.

## T9. Fixed-Spread Baseline Strategy [done]

**Goal:** implement a simple sanity baseline.

**Implemented:**

- `FixedSpreadStrategy`.
- symmetric bid/ask around mid.
- inventory guard.
- maker-only risk validation through the CLI.
- `baseline_fixed_spread.yaml`.

**DoD:** intent generation, inventory guard, invalid config rejection, sample
artifact smoke coverage, and synthetic positive spread PnL passed.

## T10. Avellaneda-Stoikov Strategy [done]

**Goal:** implement the classic 2008 A-S reservation-price strategy.

**Implemented:**

- reservation price formula;
- optimal spread approximation;
- rolling mid-return volatility;
- horizon decay;
- min spread and inventory guard;
- `avellaneda_stoikov.yaml`.

**DoD:** formula tests, gamma/sigma spread widening tests, horizon limit test,
sample artifact smoke coverage, and maker-only validation passed.

## T11. Microprice Extension [done]

**Goal:** adjust A-S fair price with order-book imbalance/microprice.

**Implemented:**

- `fair_price_mode: microprice_proxy`.
- `microprice_alpha` and `microprice_beta`.
- fair price formula:

```text
microprice_proxy = mid + alpha * (spread / 2) * imbalance
fair_price = mid + beta * (microprice_proxy - mid)
```

- `microprice_as.yaml`.

**DoD:** beta-zero equivalence to classic A-S, bid-heavy and ask-heavy
imbalance tests, sample artifact smoke coverage, and CLI aliases passed.

## T12. CLI And Configuration [done]

**Goal:** make the executable reproducible and configurable.

**Implemented:**

- repeated `--override key=value`;
- `--json` run summary;
- config-driven log level;
- plan-style YAML aliases;
- stable full-precision config hashing;
- worktree-aware git commit metadata;
- `run_metadata.json`.

**DoD:** all three configs run, overrides work, metadata contains config hash
and git commit, invalid overrides do not mutate original config, and linked
worktree git metadata tests passed.

## T13. Reporting Layer [done]

**Goal:** provide interactive and static result inspection.

**Implemented:**

- `scripts/python/dashboard.py` Streamlit dashboard.
- `scripts/python/reporting.py` readers, transformations, and Plotly figures.
- `scripts/python/export_static.py` static Markdown/PNG export.
- tracked clean-checkout sample report fixtures under `data/sample_reports/`.
- dashboard screenshot asset.

**DoD:** dashboard renders all seven tabs, compare mode works, sensitivity
heatmaps render for grid runs, static export works, README includes viewing
instructions, and Python dependencies are documented.

## T14. Experiments And Final Report [done]

**Goal:** produce experiment results for submission.

**Implemented:**

- `scripts/python/run_experiments.py`.
- three base runs: fixed spread, classic A-S, microprice A-S.
- 3x3x3 sensitivity grid over `gamma/k/beta`.
- local generated artifacts under ignored `reports/`.
- static sensitivity heatmaps copied to tracked `docs/assets/`.
- final report in `docs/report.md`.
- experiment runbook in `docs/experiments.md`.

**Result summary:**

- fixed spread: PnL `-741593`;
- A-S: PnL `-4345`;
- microprice A-S: PnL `-4345`;
- best grid run: `gamma=0.02`, `k=1.0`, `beta=1.0`, PnL `77`.

**DoD:** base artifacts, grid artifacts, report sections, sensitivity heatmaps,
and README reproduction commands are complete.

## T15. Submission Package [done]

**Status:** done on 2026-05-08 for repository-side deliverables. The final
application form submission is an external owner action.

**Goal:** package everything needed for final submission.

**Checklist:**

- [x] README with run instructions.
- [x] configs for all three strategies.
- [x] source code with engine and strategies.
- [x] unit and integration tests.
- [x] `data/sample/`.
- [x] final report.
- [x] `orders.csv`, `fills.csv`, `equity_curve.csv`, and `metrics.json` for
  each base run.
- [x] charts.
- [x] execution limitation section.
- [x] roadmap.
- [x] final clean-copy smoke test.

**DoD:**

- [x] All repository-side submission checklist items are complete.
- [x] Clean-copy smoke test passes.
- [ ] Application form submission is done by the repository owner.

**Result:** added `scripts/python/package_submission.py` and generated the local
ignored package at `submission/cmf_lob_backtester`. The package includes the
submission README, project docs, configs, source, tests, sample data, final
report, base run artifacts, and sensitivity charts. A clean-package build,
`ctest`, and sample CLI run passed from the packaged files.

## Optional Roadmap

These items are not required for the MVP and are documented in the final report
roadmap.

### O1. Learned Microprice

Replace the proxy formula with an empirical microprice estimate such as a
transition matrix or a one-tick-ahead model.

### O2. Queue Position Model

Estimate queue position from Market-By-Price data and reduce optimistic fills.

### O3. Partial Fills

Support partial fills by available volume or trade size and expose
`partial_fill_count` / `avg_fills_per_order` metrics.

### O4. Market Impact

Apply a minimal temporary impact model when owned orders fill.

## Open Decisions

1. **Raw data format:** resolved. The archive contains CSV-only `lob.csv` and
   `trades.csv`.
2. **DataLoader:** resolved. C++ streams CSV directly, merges by timestamp, and
   stores prices/quantities as integer ticks/lots.
3. **Binary preprocessing:** deferred until CSV parsing is proven to be the
   bottleneck.
4. **Walk-forward split:** deferred for committed sample data; supported as a
   future workflow with a larger local raw dataset.
5. **Learned microprice:** roadmap item; MVP uses the proxy formula.
