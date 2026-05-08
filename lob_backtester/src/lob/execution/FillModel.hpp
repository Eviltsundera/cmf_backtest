#pragma once

#include "lob/book/OrderBook.hpp"
#include "lob/data/MarketEvent.hpp"
#include "lob/execution/OrderManager.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace lob::execution {

enum class FillReference : std::uint8_t {
  TradePrice = 0,
  BestQuote = 1,
  MidPrice = 2,
};

enum class LiquidityRole : std::uint8_t {
  Maker = 0,
  Taker = 1,
};

struct FillModelConfig {
  FillReference fill_reference = FillReference::TradePrice;
  LiquidityRole default_role = LiquidityRole::Maker;
  double maker_bps = 0.0;
  double taker_bps = 0.0;
  bool partial_fills = false;
  std::int64_t latency_ns = 0;
};

struct Fill {
  OrderId order_id = 0;
  StrategyId strategy_id = 0;
  OrderSide side = OrderSide::Buy;
  Price limit_price_ticks = 0;
  Price fill_price_ticks = 0;
  Quantity quantity_lots = 0;
  std::int64_t ts_ns = 0;
  LiquidityRole liquidity_role = LiquidityRole::Maker;
  double fee = 0.0;
};

class FillModel {
public:
  explicit FillModel(FillModelConfig config = {});

  [[nodiscard]] std::optional<Fill> check_fill(const Order &order, const data::MarketEvent &event,
                                               const book::OrderBook &book) const;
  std::vector<Fill> fill_active_orders(OrderManager &orders, const data::MarketEvent &event,
                                       const book::OrderBook &book) const;

private:
  [[nodiscard]] std::optional<double> reference_price(OrderSide side,
                                                      const data::MarketEvent &event,
                                                      const book::OrderBook &book) const;
  [[nodiscard]] std::optional<double>
  trade_or_best_quote_reference(OrderSide side, const data::MarketEvent &event,
                                const book::OrderBook &book) const;
  [[nodiscard]] std::optional<double> best_quote_reference(OrderSide side,
                                                           const book::OrderBook &book) const;
  [[nodiscard]] double fee_for_fill(Price fill_price_ticks, Quantity quantity_lots) const;

  FillModelConfig config_;
};

[[nodiscard]] const char *to_string(FillReference reference);
[[nodiscard]] const char *to_string(LiquidityRole role);

} // namespace lob::execution
