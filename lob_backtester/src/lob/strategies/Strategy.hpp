#pragma once

#include "lob/book/OrderBook.hpp"
#include "lob/execution/FillModel.hpp"
#include "lob/execution/OrderManager.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
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

struct FixedSpreadStrategyConfig {
  execution::StrategyId strategy_id = 1;
  book::Price delta_ticks = 1;
  execution::Quantity order_quantity_lots = 1;
  execution::Quantity max_inventory_lots = 1;
};

class FixedSpreadStrategy final : public IStrategy {
public:
  explicit FixedSpreadStrategy(FixedSpreadStrategyConfig config);

  std::vector<execution::OrderIntent> on_market_event(const data::MarketEvent &event,
                                                      const MarketState &state) override;
  std::vector<execution::OrderIntent> on_fill(const execution::Fill &fill,
                                              const MarketState &state) override;

private:
  FixedSpreadStrategyConfig config_;
};

enum class FairPriceMode {
  Mid,
  MicropriceProxy,
};

struct AvellanedaStoikovStrategyConfig {
  execution::StrategyId strategy_id = 1;
  double gamma = 0.0;
  double initial_sigma = 0.0;
  double k = 0.0;
  double horizon_seconds = 0.0;
  std::int64_t sigma_window_ms = 0;
  book::Price min_spread_ticks = 1;
  execution::Quantity order_quantity_lots = 1;
  execution::Quantity max_inventory_lots = 1;
  FairPriceMode fair_price_mode = FairPriceMode::Mid;
  double microprice_alpha = 0.0;
  double microprice_beta = 0.0;
};

struct AvellanedaStoikovQuote {
  double reservation_price = 0.0;
  double total_spread = 0.0;
  book::Price bid_price = 0;
  book::Price ask_price = 0;
};

[[nodiscard]] AvellanedaStoikovQuote
compute_avellaneda_stoikov_quote(double fair_price, execution::Quantity inventory_lots,
                                 double gamma, double sigma, double k,
                                 double remaining_horizon_seconds, book::Price min_spread_ticks);

[[nodiscard]] double compute_microprice_adjusted_fair_price(double mid_price,
                                                            double microprice_proxy, double beta);

class AvellanedaStoikovStrategy final : public IStrategy {
public:
  explicit AvellanedaStoikovStrategy(AvellanedaStoikovStrategyConfig config);

  std::vector<execution::OrderIntent> on_market_event(const data::MarketEvent &event,
                                                      const MarketState &state) override;
  std::vector<execution::OrderIntent> on_fill(const execution::Fill &fill,
                                              const MarketState &state) override;

private:
  std::optional<double> push_mid_return_std(std::int64_t ts_ns, double mid_price);
  [[nodiscard]] double current_sigma(double mid_price) const;
  [[nodiscard]] double remaining_horizon_seconds(std::int64_t ts_ns);

  AvellanedaStoikovStrategyConfig config_;
  std::optional<std::int64_t> start_ts_ns_;
  std::optional<double> previous_mid_;
  std::deque<std::pair<std::int64_t, double>> mid_returns_;
  long double return_sum_ = 0.0L;
  long double return_square_sum_ = 0.0L;
};

} // namespace lob::strategies
