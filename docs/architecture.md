# Architecture

The backtester is an event-driven C++20 application. A run reads normalized CSV
events, applies them to a Market-By-Price order book, checks active orders for
fills, updates portfolio and metrics, then gives the strategy a current market
state.

## Runtime Flow

1. `CsvDataSource` merges `lob.csv` snapshots and `trades.csv` prints by
   timestamp and sequence.
2. `OrderBook` applies snapshots and optional depth updates, enforcing the
   configured crossed-book policy.
3. `FillModel` evaluates active orders against the current event and book.
4. `Portfolio` applies accepted fills to cash, realized PnL, and inventory.
5. `MetricsEngine` records fills, quotes, equity, inventory, drawdown, and
   adverse-selection markouts.
6. `IStrategy` receives `MarketState` callbacks and returns order intents.
7. `OrderManager` validates intents, applies maker-only and inventory gates,
   and writes the order lifecycle log.

## Module Boundaries

| Module | Responsibility |
| --- | --- |
| `data/` | CSV parsing, validation, timestamp merge, event counters. |
| `book/` | MBP book state, top-of-book features, crossed-book recovery. |
| `features/` | Mid, spread, imbalance, weighted mid, microprice proxy. |
| `execution/` | Order lifecycle, risk checks, fills, fill fees. |
| `portfolio/` | Cash, inventory, realized/unrealized/total PnL. |
| `metrics/` | Scalar metrics and run artifact writers. |
| `strategies/` | Noop, fixed spread, A-S, microprice A-S. |
| `engine/` | Event loop orchestration and output writing. |
| `utils/` | YAML config, overrides, config hash, git metadata. |

## Reproducibility

Every CLI run writes `run_metadata.json` with the effective config hash, git
commit, UTC timestamp, config path, and overrides. The hash includes the values
that affect runtime behavior, including book policy and strategy parameters.
