#pragma once

#include "lob/book/OrderBook.hpp"
#include "lob/execution/FillModel.hpp"
#include "lob/execution/OrderManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lob::strategies {

struct MarketState {
  std::int64_t ts_ns = 0;
  std::uint64_t event_seq = 0;
  std::optional<book::BookLevel> best_bid;
  std::optional<book::BookLevel> best_ask;
  std::optional<double> mid_price;
  std::optional<book::Price> spread_ticks;
  std::optional<double> imbalance;
  std::optional<double> weighted_mid;
  std::optional<double> microprice_proxy;
  execution::Quantity inventory_lots = 0;
  double cash = 0.0;
  std::size_t active_order_count = 0;
};

class IStrategy {
public:
  virtual ~IStrategy() = default;

  virtual std::vector<execution::OrderIntent> on_market_event(const data::MarketEvent &event,
                                                              const MarketState &state) = 0;
  virtual std::vector<execution::OrderIntent> on_fill(const execution::Fill &fill,
                                                      const MarketState &state) = 0;
};

class NoopStrategy final : public IStrategy {
public:
  std::vector<execution::OrderIntent> on_market_event(const data::MarketEvent &event,
                                                      const MarketState &state) override;
  std::vector<execution::OrderIntent> on_fill(const execution::Fill &fill,
                                              const MarketState &state) override;
};

} // namespace lob::strategies
