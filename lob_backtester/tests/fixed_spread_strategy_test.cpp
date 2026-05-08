#include "lob/strategies/Strategy.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

lob::strategies::FixedSpreadStrategyConfig fixed_spread_config() {
  lob::strategies::FixedSpreadStrategyConfig config;
  config.strategy_id = 42;
  config.delta_ticks = 1;
  config.order_quantity_lots = 2;
  config.max_inventory_lots = 10;
  return config;
}

lob::data::MarketEvent market_event(const std::int64_t ts_ns) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.seq = 7;
  event.type = lob::data::EventType::Snapshot;
  return event;
}

lob::strategies::MarketState market_state(const double mid_price,
                                          const lob::execution::Quantity inventory_lots = 0) {
  lob::strategies::MarketState state;
  state.mid_price = mid_price;
  state.inventory_lots = inventory_lots;
  return state;
}

} // namespace

TEST(FixedSpreadStrategyTest, GeneratesCancelAllBidAndAskAroundMid) {
  lob::strategies::FixedSpreadStrategy strategy(fixed_spread_config());

  const auto intents = strategy.on_market_event(market_event(1'000), market_state(100.0));

  ASSERT_EQ(intents.size(), 3U);
  EXPECT_EQ(intents[0].type, lob::execution::OrderIntentType::CancelAll);
  EXPECT_EQ(intents[0].strategy_id, 42U);
  EXPECT_EQ(intents[0].ts_ns, 1'000);

  EXPECT_EQ(intents[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(intents[1].strategy_id, 42U);
  EXPECT_EQ(intents[1].side, lob::execution::OrderSide::Buy);
  EXPECT_EQ(intents[1].price_ticks, 99);
  EXPECT_EQ(intents[1].quantity_lots, 2);

  EXPECT_EQ(intents[2].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(intents[2].strategy_id, 42U);
  EXPECT_EQ(intents[2].side, lob::execution::OrderSide::Sell);
  EXPECT_EQ(intents[2].price_ticks, 101);
  EXPECT_EQ(intents[2].quantity_lots, 2);
}

TEST(FixedSpreadStrategyTest, StopsQuotingSideThatWouldBreachInventoryLimit) {
  lob::strategies::FixedSpreadStrategy strategy(fixed_spread_config());

  const auto long_full = strategy.on_market_event(market_event(2'000), market_state(100.0, 9));
  ASSERT_EQ(long_full.size(), 2U);
  EXPECT_EQ(long_full[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(long_full[1].side, lob::execution::OrderSide::Sell);

  const auto short_full = strategy.on_market_event(market_event(3'000), market_state(100.0, -9));
  ASSERT_EQ(short_full.size(), 2U);
  EXPECT_EQ(short_full[1].type, lob::execution::OrderIntentType::SubmitLimit);
  EXPECT_EQ(short_full[1].side, lob::execution::OrderSide::Buy);
}

TEST(FixedSpreadStrategyTest, RejectsInvalidConfig) {
  auto config = fixed_spread_config();
  config.delta_ticks = 0;
  EXPECT_THROW(static_cast<void>(lob::strategies::FixedSpreadStrategy{config}), std::runtime_error);

  config = fixed_spread_config();
  config.order_quantity_lots = 0;
  EXPECT_THROW(static_cast<void>(lob::strategies::FixedSpreadStrategy{config}), std::runtime_error);

  config = fixed_spread_config();
  config.max_inventory_lots = -1;
  EXPECT_THROW(static_cast<void>(lob::strategies::FixedSpreadStrategy{config}), std::runtime_error);

  config = fixed_spread_config();
  config.max_inventory_lots = config.order_quantity_lots - 1;
  EXPECT_THROW(static_cast<void>(lob::strategies::FixedSpreadStrategy{config}), std::runtime_error);
}
