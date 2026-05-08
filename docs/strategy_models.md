# Strategy Models

The strategy interface receives a current `MarketState` and returns order
intents. Production configs use maker-only validation in the `OrderManager`.

## Noop

`noop` emits no order intents. It is useful for data replay, throughput checks,
and artifact smoke tests.

## Fixed Spread

`fixed_spread` quotes one tick distance parameter around the current mid:

```text
bid = floor(mid - delta_ticks)
ask = ceil(mid + delta_ticks)
```

Inventory gates stop quoting the side that would exceed `max_inventory_lots`.
The strategy is deliberately naive and is used as an adverse-selection
baseline.

## Avellaneda-Stoikov

The classic A-S strategy computes a reservation price and spread:

```text
reservation = fair_price - inventory * gamma * sigma^2 * remaining_horizon
spread = gamma * sigma^2 * remaining_horizon + (2 / gamma) * log(1 + gamma / k)
```

The implementation estimates `sigma` online from rolling mid returns, with
`strategy.sigma` used as the initial value until enough observations arrive.

## Microprice A-S

`microprice_as` changes only the fair-price input:

```text
microprice_proxy = mid + microprice_alpha * (spread / 2) * imbalance
fair_price = mid + microprice_beta * (microprice_proxy - mid)
```

When `microprice_beta = 0`, the strategy intentionally reduces to classic A-S
and does not require a usable imbalance signal for the fair-price adjustment.
For `microprice_beta > 0`, missing mid, spread, or imbalance means the strategy
cancels existing orders and skips new quotes for that event.

## One-Sided Quoting

The maker-only gate can reject a generated bid or ask if A-S inventory skew
pushes that quote through the current touch. Under the sample config this is
visible as low two-sided quote uptime for A-S variants. This is a model/config
effect rather than a metrics failure.
