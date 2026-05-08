#include "lob/strategies/Strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace lob::strategies {
namespace {

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;

book::Price round_down_price(const double price) {
  if (!std::isfinite(price) ||
      price < static_cast<double>(std::numeric_limits<book::Price>::min()) ||
      price > static_cast<double>(std::numeric_limits<book::Price>::max())) {
    throw std::runtime_error("Strategy quote price is out of range");
  }
  return static_cast<book::Price>(std::floor(price));
}

book::Price round_up_price(const double price) {
  if (!std::isfinite(price) ||
      price < static_cast<double>(std::numeric_limits<book::Price>::min()) ||
      price > static_cast<double>(std::numeric_limits<book::Price>::max())) {
    throw std::runtime_error("Strategy quote price is out of range");
  }
  return static_cast<book::Price>(std::ceil(price));
}

bool can_quote_buy(const execution::Quantity max_inventory_lots,
                   const execution::Quantity order_quantity_lots,
                   const execution::Quantity inventory_lots) {
  return inventory_lots <= max_inventory_lots - order_quantity_lots;
}

bool can_quote_sell(const execution::Quantity max_inventory_lots,
                    const execution::Quantity order_quantity_lots,
                    const execution::Quantity inventory_lots) {
  return inventory_lots >= -max_inventory_lots + order_quantity_lots;
}

void validate_order_sizing(const execution::Quantity order_quantity_lots,
                           const execution::Quantity max_inventory_lots,
                           const char *strategy_name) {
  if (order_quantity_lots <= 0) {
    throw std::runtime_error(std::string(strategy_name) + " order_qty must be positive");
  }
  if (max_inventory_lots < 0) {
    throw std::runtime_error(std::string(strategy_name) + " max_inventory must be non-negative");
  }
  if (max_inventory_lots < order_quantity_lots) {
    throw std::runtime_error(std::string(strategy_name) + " max_inventory must be >= order_qty");
  }
}

bool is_maker_buy(const MarketState &state, const book::Price price_ticks) {
  return state.best_ask && price_ticks < state.best_ask->price_ticks;
}

bool is_maker_sell(const MarketState &state, const book::Price price_ticks) {
  return state.best_bid && price_ticks > state.best_bid->price_ticks;
}

double population_std(const std::size_t count, const long double sum,
                      const long double square_sum) {
  const long double count_value = static_cast<long double>(count);
  const long double mean = sum / count_value;
  const long double variance = std::max(0.0L, (square_sum / count_value) - (mean * mean));
  return std::sqrt(static_cast<double>(variance));
}

bool is_positive_finite_price(const double price) {
  return std::isfinite(price) && price > 0.0;
}

std::optional<double> safe_microprice_adjusted_fair_price(const double mid_price,
                                                          const double microprice_proxy,
                                                          const double beta) {
  if (!is_positive_finite_price(mid_price) || !is_positive_finite_price(microprice_proxy) ||
      !std::isfinite(beta) || beta < 0.0) {
    return std::nullopt;
  }

  const double fair_price = mid_price + (beta * (microprice_proxy - mid_price));
  if (!is_positive_finite_price(fair_price)) {
    return std::nullopt;
  }
  return fair_price;
}

std::optional<double> microprice_proxy_from_state(const MarketState &state, const double alpha) {
  if (!state.mid_price || !is_positive_finite_price(*state.mid_price) || !state.spread_ticks ||
      *state.spread_ticks <= 0 || !state.imbalance || !std::isfinite(*state.imbalance)) {
    return std::nullopt;
  }

  const double proxy =
      *state.mid_price +
      (alpha * (static_cast<double>(*state.spread_ticks) / 2.0) * *state.imbalance);
  if (!is_positive_finite_price(proxy)) {
    return std::nullopt;
  }
  return proxy;
}

std::optional<double> fair_price_from_state(const MarketState &state,
                                            const AvellanedaStoikovStrategyConfig &config) {
  if (!state.mid_price || !is_positive_finite_price(*state.mid_price)) {
    return std::nullopt;
  }

  if (config.fair_price_mode == FairPriceMode::Mid) {
    return *state.mid_price;
  }
  if (config.fair_price_mode == FairPriceMode::MicropriceProxy) {
    if (config.microprice_beta == 0.0) {
      return *state.mid_price;
    }
    const auto proxy = microprice_proxy_from_state(state, config.microprice_alpha);
    if (!proxy) {
      return std::nullopt;
    }
    return safe_microprice_adjusted_fair_price(*state.mid_price, *proxy, config.microprice_beta);
  }

  throw std::runtime_error("Unsupported AvellanedaStoikov fair_price_mode");
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
  validate_order_sizing(config_.order_quantity_lots, config_.max_inventory_lots,
                        "FixedSpreadStrategy");
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
  const book::Price bid_price = round_down_price(*state.mid_price - delta);
  const book::Price ask_price = round_up_price(*state.mid_price + delta);
  if (bid_price <= 0 || ask_price <= 0 || bid_price >= ask_price) {
    return intents;
  }

  if (can_quote_buy(config_.max_inventory_lots, config_.order_quantity_lots,
                    state.inventory_lots)) {
    intents.push_back(
        execution::OrderIntent::submit_limit(config_.strategy_id, execution::OrderSide::Buy,
                                             bid_price, config_.order_quantity_lots, event.ts_ns));
  }
  if (can_quote_sell(config_.max_inventory_lots, config_.order_quantity_lots,
                     state.inventory_lots)) {
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

AvellanedaStoikovQuote compute_avellaneda_stoikov_quote(const double fair_price,
                                                        const execution::Quantity inventory_lots,
                                                        const double gamma, const double sigma,
                                                        const double k,
                                                        const double remaining_horizon_seconds,
                                                        const book::Price min_spread_ticks) {
  if (!is_positive_finite_price(fair_price)) {
    throw std::runtime_error("AvellanedaStoikov fair_price must be positive and finite");
  }
  if (!std::isfinite(gamma) || gamma <= 0.0) {
    throw std::runtime_error("AvellanedaStoikov gamma must be positive and finite");
  }
  if (!std::isfinite(sigma) || sigma < 0.0) {
    throw std::runtime_error("AvellanedaStoikov sigma must be non-negative and finite");
  }
  if (!std::isfinite(k) || k <= 0.0) {
    throw std::runtime_error("AvellanedaStoikov k must be positive and finite");
  }
  if (!std::isfinite(remaining_horizon_seconds) || remaining_horizon_seconds < 0.0) {
    throw std::runtime_error("AvellanedaStoikov horizon must be non-negative and finite");
  }
  if (min_spread_ticks <= 0) {
    throw std::runtime_error("AvellanedaStoikov min_spread_ticks must be positive");
  }

  const double sigma_squared = sigma * sigma;
  const double inventory_risk = gamma * sigma_squared * remaining_horizon_seconds;
  const double reservation_price =
      fair_price - (static_cast<double>(inventory_lots) * inventory_risk);
  const double liquidity_spread = (2.0 / gamma) * std::log1p(gamma / k);
  const double model_spread = inventory_risk + liquidity_spread;
  if (!std::isfinite(reservation_price) || !std::isfinite(model_spread)) {
    throw std::runtime_error("AvellanedaStoikov quote calculation overflowed");
  }

  const double total_spread = std::max(static_cast<double>(min_spread_ticks), model_spread);
  AvellanedaStoikovQuote quote;
  quote.reservation_price = reservation_price;
  quote.total_spread = total_spread;
  quote.bid_price = round_down_price(reservation_price - (total_spread / 2.0));
  quote.ask_price = round_up_price(reservation_price + (total_spread / 2.0));
  return quote;
}

double compute_microprice_adjusted_fair_price(const double mid_price, const double microprice_proxy,
                                              const double beta) {
  const auto fair_price = safe_microprice_adjusted_fair_price(mid_price, microprice_proxy, beta);
  if (!fair_price) {
    throw std::runtime_error(
        "Microprice fair price inputs must be positive, finite, and have non-negative beta");
  }
  return *fair_price;
}

AvellanedaStoikovStrategy::AvellanedaStoikovStrategy(AvellanedaStoikovStrategyConfig config)
    : config_(config) {
  if (!std::isfinite(config_.gamma) || config_.gamma <= 0.0) {
    throw std::runtime_error("AvellanedaStoikovStrategy gamma must be positive and finite");
  }
  if (!std::isfinite(config_.initial_sigma) || config_.initial_sigma < 0.0) {
    throw std::runtime_error(
        "AvellanedaStoikovStrategy initial sigma must be non-negative and finite");
  }
  if (!std::isfinite(config_.k) || config_.k <= 0.0) {
    throw std::runtime_error("AvellanedaStoikovStrategy k must be positive and finite");
  }
  if (!std::isfinite(config_.horizon_seconds) || config_.horizon_seconds < 0.0) {
    throw std::runtime_error(
        "AvellanedaStoikovStrategy horizon_seconds must be non-negative and finite");
  }
  if (config_.sigma_window_ms <= 0) {
    throw std::runtime_error("AvellanedaStoikovStrategy sigma_window_ms must be positive");
  }
  if (config_.sigma_window_ms >
      std::numeric_limits<std::int64_t>::max() / kNanosecondsPerMillisecond) {
    throw std::runtime_error("AvellanedaStoikovStrategy sigma_window_ms overflows nanoseconds");
  }
  if (config_.min_spread_ticks <= 0) {
    throw std::runtime_error("AvellanedaStoikovStrategy min_spread_ticks must be positive");
  }
  if (!std::isfinite(config_.microprice_alpha) || config_.microprice_alpha < 0.0) {
    throw std::runtime_error(
        "AvellanedaStoikovStrategy microprice_alpha must be non-negative and finite");
  }
  if (!std::isfinite(config_.microprice_beta) || config_.microprice_beta < 0.0) {
    throw std::runtime_error(
        "AvellanedaStoikovStrategy microprice_beta must be non-negative and finite");
  }
  validate_order_sizing(config_.order_quantity_lots, config_.max_inventory_lots,
                        "AvellanedaStoikovStrategy");
}

std::vector<execution::OrderIntent>
AvellanedaStoikovStrategy::on_market_event(const data::MarketEvent &event,
                                           const MarketState &state) {
  std::vector<execution::OrderIntent> intents;
  intents.reserve(3);
  intents.push_back(execution::OrderIntent::cancel_all(config_.strategy_id, event.ts_ns));

  if (!state.mid_price || !std::isfinite(*state.mid_price) || *state.mid_price <= 0.0) {
    return intents;
  }

  const auto fair_price = fair_price_from_state(state, config_);
  if (!fair_price) {
    return intents;
  }

  push_mid_return_std(event.ts_ns, *state.mid_price);
  const AvellanedaStoikovQuote quote = compute_avellaneda_stoikov_quote(
      *fair_price, state.inventory_lots, config_.gamma, current_sigma(*state.mid_price), config_.k,
      remaining_horizon_seconds(event.ts_ns), config_.min_spread_ticks);

  if (quote.bid_price <= 0 || quote.ask_price <= 0 || quote.bid_price >= quote.ask_price) {
    return intents;
  }

  if (can_quote_buy(config_.max_inventory_lots, config_.order_quantity_lots,
                    state.inventory_lots) &&
      is_maker_buy(state, quote.bid_price)) {
    intents.push_back(execution::OrderIntent::submit_limit(
        config_.strategy_id, execution::OrderSide::Buy, quote.bid_price,
        config_.order_quantity_lots, event.ts_ns));
  }
  if (can_quote_sell(config_.max_inventory_lots, config_.order_quantity_lots,
                     state.inventory_lots) &&
      is_maker_sell(state, quote.ask_price)) {
    intents.push_back(execution::OrderIntent::submit_limit(
        config_.strategy_id, execution::OrderSide::Sell, quote.ask_price,
        config_.order_quantity_lots, event.ts_ns));
  }

  return intents;
}

std::vector<execution::OrderIntent> AvellanedaStoikovStrategy::on_fill(const execution::Fill &,
                                                                       const MarketState &) {
  return {};
}

std::optional<double> AvellanedaStoikovStrategy::push_mid_return_std(const std::int64_t ts_ns,
                                                                     const double mid_price) {
  if (!std::isfinite(mid_price) || mid_price <= 0.0) {
    throw std::runtime_error("AvellanedaStoikovStrategy mid_price must be positive and finite");
  }

  if (!previous_mid_) {
    previous_mid_ = mid_price;
    return std::nullopt;
  }

  const double mid_return = (mid_price - *previous_mid_) / *previous_mid_;
  previous_mid_ = mid_price;
  mid_returns_.emplace_back(ts_ns, mid_return);
  return_sum_ += mid_return;
  return_square_sum_ += static_cast<long double>(mid_return) * static_cast<long double>(mid_return);

  const std::int64_t window_ns = config_.sigma_window_ms * kNanosecondsPerMillisecond;
  while (!mid_returns_.empty() && ts_ns - mid_returns_.front().first > window_ns) {
    const double oldest = mid_returns_.front().second;
    mid_returns_.pop_front();
    return_sum_ -= oldest;
    return_square_sum_ -= static_cast<long double>(oldest) * static_cast<long double>(oldest);
  }

  if (mid_returns_.size() < 2) {
    return std::nullopt;
  }
  return population_std(mid_returns_.size(), return_sum_, return_square_sum_);
}

double AvellanedaStoikovStrategy::current_sigma(const double mid_price) const {
  if (mid_returns_.size() < 2) {
    return config_.initial_sigma;
  }
  return population_std(mid_returns_.size(), return_sum_, return_square_sum_) * mid_price;
}

double AvellanedaStoikovStrategy::remaining_horizon_seconds(const std::int64_t ts_ns) {
  if (!start_ts_ns_) {
    start_ts_ns_ = ts_ns;
  }

  const double elapsed_seconds =
      std::max(0.0, static_cast<double>(ts_ns - *start_ts_ns_) / kNanosecondsPerSecond);
  return std::max(0.0, config_.horizon_seconds - elapsed_seconds);
}

} // namespace lob::strategies
