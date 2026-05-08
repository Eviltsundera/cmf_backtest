#include "lob/strategies/Strategy.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

lob::strategies::AvellanedaStoikovStrategyConfig avellaneda_config() {
  lob::strategies::AvellanedaStoikovStrategyConfig config;
  config.strategy_id = 77;
  config.gamma = 0.01;
  config.initial_sigma = 0.0;
  config.k = 1.0;
  config.horizon_seconds = 60.0;
  config.sigma_window_ms = 1'000;
  config.min_spread_ticks = 2;
  config.order_quantity_lots = 2;
  config.max_inventory_lots = 10;
  return config;
}

lob::data::MarketEvent market_event(const std::int64_t ts_ns) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.seq = 11;
  event.type = lob::data::EventType::Snapshot;
  return event;
}

lob::strategies::MarketState market_state(const double mid_price,
                                          const lob::execution::Quantity inventory_lots = 0,
                                          const lob::book::Price best_bid = 99,
                                          const lob::book::Price best_ask = 101,
                                          const lob::book::Quantity bid_quantity = 10,
                                          const lob::book::Quantity ask_quantity = 10) {
  lob::strategies::MarketState state;
  state.mid_price = mid_price;
  state.best_bid = lob::book::BookLevel{.price_ticks = best_bid, .quantity_lots = bid_quantity};
  state.best_ask = lob::book::BookLevel{.price_ticks = best_ask, .quantity_lots = ask_quantity};
  state.spread_ticks = best_ask - best_bid;
  state.imbalance = static_cast<double>(bid_quantity - ask_quantity) /
                    static_cast<double>(bid_quantity + ask_quantity);
  state.microprice_proxy =
      mid_price + (static_cast<double>(*state.spread_ticks) / 2.0) * *state.imbalance;
  state.inventory_lots = inventory_lots;
  return state;
}

void expect_intents_equal(const std::vector<lob::execution::OrderIntent> &left,
                          const std::vector<lob::execution::OrderIntent> &right) {
  ASSERT_EQ(left.size(), right.size());
  for (std::size_t index = 0; index < left.size(); ++index) {
    EXPECT_EQ(left[index].type, right[index].type);
    EXPECT_EQ(left[index].strategy_id, right[index].strategy_id);
    EXPECT_EQ(left[index].order_id, right[index].order_id);
    EXPECT_EQ(left[index].side, right[index].side);
    EXPECT_EQ(left[index].price_ticks, right[index].price_ticks);
    EXPECT_EQ(left[index].quantity_lots, right[index].quantity_lots);
    EXPECT_EQ(left[index].ts_ns, right[index].ts_ns);
  }
}

} // namespace

TEST(AvellanedaStoikovFormulaTest, ReservationPriceMovesWithInventory) {
  const auto flat =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.1, 2.0, 1.0, 10.0, 1);
  const auto long_inventory =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 1, 0.1, 2.0, 1.0, 10.0, 1);
  const auto short_inventory =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, -1, 0.1, 2.0, 1.0, 10.0, 1);

  EXPECT_DOUBLE_EQ(flat.reservation_price, 100.0);
  EXPECT_LT(long_inventory.reservation_price, 100.0);
  EXPECT_GT(short_inventory.reservation_price, 100.0);
}

TEST(AvellanedaStoikovFormulaTest, SpreadWidensWithGammaOrSigma) {
  const auto low_gamma =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.05, 3.0, 1.0, 10.0, 1);
  const auto high_gamma =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.1, 3.0, 1.0, 10.0, 1);
  EXPECT_GT(high_gamma.total_spread, low_gamma.total_spread);

  const auto low_sigma =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.05, 1.0, 1.0, 10.0, 1);
  const auto high_sigma =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.05, 2.0, 1.0, 10.0, 1);
  EXPECT_GT(high_sigma.total_spread, low_sigma.total_spread);
}

TEST(AvellanedaStoikovFormulaTest, ZeroRemainingHorizonLeavesOnlyLiquiditySpread) {
  constexpr double gamma = 0.2;
  constexpr double k = 1.5;
  const auto quote =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 5, gamma, 3.0, k, 0.0, 1);
  const double expected_spread = (2.0 / gamma) * std::log1p(gamma / k);

  EXPECT_DOUBLE_EQ(quote.reservation_price, 100.0);
  EXPECT_NEAR(quote.total_spread, expected_spread, 1e-12);
}

TEST(MicropriceAvellanedaStoikovFormulaTest, BidHeavyImbalanceRaisesReservationPrice) {
  const auto state = market_state(100.0, 0, 99, 101, 30, 10);
  const auto classic =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.1, 1.0, 1.0, 10.0, 1);
  const double fair_price = lob::strategies::compute_microprice_adjusted_fair_price(
      *state.mid_price, *state.microprice_proxy, 1.0);
  const auto microprice =
      lob::strategies::compute_avellaneda_stoikov_quote(fair_price, 0, 0.1, 1.0, 1.0, 10.0, 1);

  EXPECT_GT(microprice.reservation_price, classic.reservation_price);
}

TEST(MicropriceAvellanedaStoikovFormulaTest, AskHeavyImbalanceLowersReservationPrice) {
  const auto state = market_state(100.0, 0, 99, 101, 10, 30);
  const auto classic =
      lob::strategies::compute_avellaneda_stoikov_quote(100.0, 0, 0.1, 1.0, 1.0, 10.0, 1);
  const double fair_price = lob::strategies::compute_microprice_adjusted_fair_price(
      *state.mid_price, *state.microprice_proxy, 1.0);
  const auto microprice =
      lob::strategies::compute_avellaneda_stoikov_quote(fair_price, 0, 0.1, 1.0, 1.0, 10.0, 1);

  EXPECT_LT(microprice.reservation_price, classic.reservation_price);
}

TEST(AvellanedaStoikovStrategyTest, GeneratesCancelAllBidAndAskAroundReservationPrice) {
  lob::strategies::AvellanedaStoikovStrategy strategy(avellaneda_config());

  const auto intents = strategy.on_market_event(market_event(1'000), market_state(100.0));

  ASSERT_EQ(intents.size(), 3U);
  EXPECT_EQ(intents[0].type, lob::execution::OrderIntentType::CancelAll);
  EXPECT_EQ(intents[0].strategy_id, 77U);
  EXPECT_EQ(intents[0].ts_ns, 1'000);

  EXPECT_EQ(intents[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(intents[1].side, lob::execution::OrderSide::Buy);
  EXPECT_EQ(intents[1].price_ticks, 99);
  EXPECT_EQ(intents[1].quantity_lots, 2);

  EXPECT_EQ(intents[2].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(intents[2].side, lob::execution::OrderSide::Sell);
  EXPECT_EQ(intents[2].price_ticks, 101);
  EXPECT_EQ(intents[2].quantity_lots, 2);
}

TEST(AvellanedaStoikovStrategyTest, StopsQuotingSideThatWouldBreachInventoryLimit) {
  lob::strategies::AvellanedaStoikovStrategy strategy(avellaneda_config());

  const auto long_full = strategy.on_market_event(market_event(2'000), market_state(100.0, 9));
  ASSERT_EQ(long_full.size(), 2U);
  EXPECT_EQ(long_full[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(long_full[1].side, lob::execution::OrderSide::Sell);

  const auto short_full = strategy.on_market_event(market_event(3'000), market_state(100.0, -9));
  ASSERT_EQ(short_full.size(), 2U);
  EXPECT_EQ(short_full[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(short_full[1].side, lob::execution::OrderSide::Buy);
}

TEST(AvellanedaStoikovStrategyTest, RollingMidReturnVolatilityWidensQuotes) {
  auto config = avellaneda_config();
  config.gamma = 0.01;
  config.initial_sigma = 0.0;
  config.k = 1.0;
  config.horizon_seconds = 10.0;
  config.sigma_window_ms = 1'000;
  lob::strategies::AvellanedaStoikovStrategy strategy(config);

  const auto initial =
      strategy.on_market_event(market_event(1'000'000), market_state(100.0, 0, 50, 150));
  ASSERT_EQ(initial.size(), 3U);
  const auto initial_spread = initial[2].price_ticks - initial[1].price_ticks;

  static_cast<void>(
      strategy.on_market_event(market_event(2'000'000), market_state(110.0, 0, 50, 150)));
  const auto volatile_quote =
      strategy.on_market_event(market_event(3'000'000), market_state(90.0, 0, 50, 150));
  ASSERT_EQ(volatile_quote.size(), 3U);
  const auto volatile_spread = volatile_quote[2].price_ticks - volatile_quote[1].price_ticks;

  EXPECT_GT(volatile_spread, initial_spread);
}

TEST(MicropriceAvellanedaStoikovStrategyTest, BetaZeroMatchesClassicStrategyOnSameStream) {
  auto classic_config = avellaneda_config();
  lob::strategies::AvellanedaStoikovStrategy classic_strategy(classic_config);

  auto microprice_config = avellaneda_config();
  microprice_config.fair_price_mode = lob::strategies::FairPriceMode::MicropriceProxy;
  microprice_config.microprice_alpha = 1.0;
  microprice_config.microprice_beta = 0.0;
  lob::strategies::AvellanedaStoikovStrategy microprice_strategy(microprice_config);

  const std::vector<lob::strategies::MarketState> states = {
      market_state(100.0, 0, 99, 101, 30, 10),
      market_state(101.0, 1, 100, 102, 5, 20),
      market_state(99.0, -1, 98, 100, 50, 10),
  };

  for (std::size_t index = 0; index < states.size(); ++index) {
    const auto event = market_event(static_cast<std::int64_t>((index + 1) * 1'000'000));
    expect_intents_equal(classic_strategy.on_market_event(event, states[index]),
                         microprice_strategy.on_market_event(event, states[index]));
  }
}

TEST(AvellanedaStoikovStrategyTest, RejectsInvalidConfig) {
  auto config = avellaneda_config();
  config.gamma = 0.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.initial_sigma = -1.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.k = 0.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.horizon_seconds = -1.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.sigma_window_ms = 0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.min_spread_ticks = 0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.order_quantity_lots = 0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.max_inventory_lots = config.order_quantity_lots - 1;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.fair_price_mode = lob::strategies::FairPriceMode::MicropriceProxy;
  config.microprice_alpha = -1.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);

  config = avellaneda_config();
  config.fair_price_mode = lob::strategies::FairPriceMode::MicropriceProxy;
  config.microprice_beta = -1.0;
  EXPECT_THROW(static_cast<void>(lob::strategies::AvellanedaStoikovStrategy{config}),
               std::runtime_error);
}
