# Execution Model

The current execution model is a deterministic price-cross simulator for
Market-By-Price data. It is intentionally simple and documented as a baseline,
not as a full exchange matching engine.

## Fill Rules

With `fill_reference: trade_price`, active maker orders are checked only on
trade events from the opposite aggressor side:

| Resting order | Required trade side | Price condition |
| --- | --- | --- |
| Buy limit | `sell` | `trade.price <= limit` |
| Sell limit | `buy` | `trade.price >= limit` |

For non-trade events, `trade_price` falls back to the best quote reference so
tests and synthetic streams can exercise crossing behavior without trades.
`best_quote` and `mid_price` references are also available for diagnostics.

## Order Lifecycle

`OrderManager` accepts submit, replace, cancel, and cancel-all intents. It logs
submitted, replaced, cancelled, filled, and rejected lifecycle events to
`orders.csv`. Replace is modeled as cancel old order plus submit new order.

## Risk Gates

- `strict_maker` rejects orders that would cross the current best quote.
- `max_inventory_lots` is checked against worst-case active fills on each side.
- Strict maker mode requires a current book; missing book state rejects submit
  intents instead of silently skipping validation.

## Known Limitations

- No queue position model.
- No partial fills in the submitted experiment configs.
- No submit/cancel latency or exchange acknowledgement delay.
- No market impact from strategy-owned fills.
- MBP snapshots do not expose order-level queue reconstruction.
