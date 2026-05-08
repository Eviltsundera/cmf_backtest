#include "lob/strategies/Strategy.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace lob::strategies {
namespace {

book::Price floor_price(const double price) {
  if (!std::isfinite(price) ||
      price < static_cast<double>(std::numeric_limits<book::Price>::min()) ||
      price > static_cast<double>(std::numeric_limits<book::Price>::max())) {
    throw std::runtime_error("FixedSpreadStrategy quote price is out of range");
  }
  return static_cast<book::Price>(std::floor(price));
}

book::Price ceil_price(const double price) {
  if (!std::isfinite(price) ||
      price < static_cast<double>(std::numeric_limits<book::Price>::min()) ||
      price > static_cast<double>(std::numeric_limits<book::Price>::max())) {
    throw std::runtime_error("FixedSpreadStrategy quote price is out of range");
  }
  return static_cast<book::Price>(std::ceil(price));
}

bool can_quote_buy(const FixedSpreadStrategyConfig &config,
                   const execution::Quantity inventory_lots) {
  return inventory_lots <= config.max_inventory_lots - config.order_quantity_lots;
}

bool can_quote_sell(const FixedSpreadStrategyConfig &config,
                    const execution::Quantity inventory_lots) {
  return inventory_lots >= -config.max_inventory_lots + config.order_quantity_lots;
}

} // namespace

std::vector<execution::OrderIntent> NoopStrategy::on_market_event(const data::MarketEvent &,
                                                                  const MarketState &) {
  return {};
}

std::vector<execution::OrderIntent> NoopStrategy::on_fill(const execution::Fill &,
                                                          const MarketState &) {
  return {};
}

FixedSpreadStrategy::FixedSpreadStrategy(FixedSpreadStrategyConfig config) : config_(config) {
  if (config_.delta_ticks <= 0) {
    throw std::runtime_error("FixedSpreadStrategy delta_ticks must be positive");
  }
  if (config_.order_quantity_lots <= 0) {
    throw std::runtime_error("FixedSpreadStrategy order_qty must be positive");
  }
  if (config_.max_inventory_lots < 0) {
    throw std::runtime_error("FixedSpreadStrategy max_inventory must be non-negative");
  }
  if (config_.max_inventory_lots < config_.order_quantity_lots) {
    throw std::runtime_error("FixedSpreadStrategy max_inventory must be >= order_qty");
  }
}

std::vector<execution::OrderIntent>
FixedSpreadStrategy::on_market_event(const data::MarketEvent &event, const MarketState &state) {
  std::vector<execution::OrderIntent> intents;
  intents.reserve(3);
  intents.push_back(execution::OrderIntent::cancel_all(config_.strategy_id, event.ts_ns));

  if (!state.mid_price || !std::isfinite(*state.mid_price)) {
    return intents;
  }

  const double delta = static_cast<double>(config_.delta_ticks);
  const book::Price bid_price = floor_price(*state.mid_price - delta);
  const book::Price ask_price = ceil_price(*state.mid_price + delta);
  if (bid_price <= 0 || ask_price <= 0 || bid_price >= ask_price) {
    return intents;
  }

  if (can_quote_buy(config_, state.inventory_lots)) {
    intents.push_back(
        execution::OrderIntent::submit_limit(config_.strategy_id, execution::OrderSide::Buy,
                                             bid_price, config_.order_quantity_lots, event.ts_ns));
  }
  if (can_quote_sell(config_, state.inventory_lots)) {
    intents.push_back(
        execution::OrderIntent::submit_limit(config_.strategy_id, execution::OrderSide::Sell,
                                             ask_price, config_.order_quantity_lots, event.ts_ns));
  }

  return intents;
}

std::vector<execution::OrderIntent> FixedSpreadStrategy::on_fill(const execution::Fill &,
                                                                 const MarketState &) {
  return {};
}

} // namespace lob::strategies
