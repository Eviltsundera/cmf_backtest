#include "lob/execution/FillModel.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

lob::book::OrderBook make_book() {
  lob::book::OrderBook book;
  book.apply_update(lob::data::BookSide::Bid, 99, 10);
  book.apply_update(lob::data::BookSide::Ask, 101, 10);
  return book;
}

lob::data::MarketEvent trade_event(const std::int64_t ts_ns, const std::int64_t price_ticks,
                                   const lob::data::TradeSide side = lob::data::TradeSide::Buy) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.type = lob::data::EventType::Trade;
  event.payload.trade =
      lob::data::TradePayload{.side = side, .price_ticks = price_ticks, .quantity_lots = 1};
  return event;
}

lob::data::MarketEvent depth_update_event(const std::int64_t ts_ns) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.type = lob::data::EventType::DepthUpdate;
  event.payload.depth_update = lob::data::DepthUpdatePayload{
      .side = lob::data::BookSide::Bid, .price_ticks = 99, .quantity_lots = 10};
  return event;
}

lob::execution::FillModelConfig trade_price_config() {
  lob::execution::FillModelConfig config;
  config.fill_reference = lob::execution::FillReference::TradePrice;
  config.default_role = lob::execution::LiquidityRole::Maker;
  config.maker_bps = 0.0;
  config.taker_bps = 0.0;
  return config;
}

} // namespace

TEST(FillModelTest, BuyLimitFillsWhenTradePriceIsAtOrBelowLimit) {
  lob::execution::OrderManager orders;
  const auto &order = orders.submit_limit(1, lob::execution::OrderSide::Buy, 100, 2, 1000);
  const lob::execution::FillModel model(trade_price_config());

  const auto fill = model.check_fill(order, trade_event(1100, 99), make_book());

  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->order_id, order.id);
  EXPECT_EQ(fill->fill_price_ticks, 100);
  EXPECT_EQ(fill->quantity_lots, 2);
  EXPECT_EQ(fill->fee, 0.0);
}

TEST(FillModelTest, BuyLimitDoesNotFillWhenTradePriceIsAboveLimit) {
  lob::execution::OrderManager orders;
  const auto &order = orders.submit_limit(1, lob::execution::OrderSide::Buy, 100, 2, 1000);
  const lob::execution::FillModel model(trade_price_config());

  EXPECT_FALSE(model.check_fill(order, trade_event(1100, 101), make_book()).has_value());
}

TEST(FillModelTest, SellLimitFillsWhenTradePriceIsAtOrAboveLimit) {
  lob::execution::OrderManager orders;
  const auto &order = orders.submit_limit(1, lob::execution::OrderSide::Sell, 100, 2, 1000);
  const lob::execution::FillModel model(trade_price_config());

  const auto fill = model.check_fill(order, trade_event(1100, 101), make_book());

  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->order_id, order.id);
  EXPECT_EQ(fill->fill_price_ticks, 100);
  EXPECT_EQ(fill->quantity_lots, 2);
}

TEST(FillModelTest, SellLimitDoesNotFillWhenTradePriceIsBelowLimit) {
  lob::execution::OrderManager orders;
  const auto &order = orders.submit_limit(1, lob::execution::OrderSide::Sell, 100, 2, 1000);
  const lob::execution::FillModel model(trade_price_config());

  EXPECT_FALSE(model.check_fill(order, trade_event(1100, 99), make_book()).has_value());
}

TEST(FillModelTest, FallsBackToBestQuoteWhenEventHasNoTradePrice) {
  lob::execution::OrderManager orders;
  const auto &buy = orders.submit_limit(1, lob::execution::OrderSide::Buy, 101, 2, 1000);
  const auto &sell = orders.submit_limit(1, lob::execution::OrderSide::Sell, 99, 2, 1001);
  const lob::execution::FillModel model(trade_price_config());
  const auto book = make_book();
  const auto event = depth_update_event(1200);

  EXPECT_TRUE(model.check_fill(buy, event, book).has_value());
  EXPECT_TRUE(model.check_fill(sell, event, book).has_value());
}

TEST(FillModelTest, SupportsBestQuoteMidPriceFeesAndConfigValidation) {
  lob::execution::OrderManager orders;
  const auto &buy = orders.submit_limit(1, lob::execution::OrderSide::Buy, 100, 2, 1000);

  lob::execution::FillModelConfig best_quote_config;
  best_quote_config.fill_reference = lob::execution::FillReference::BestQuote;
  EXPECT_FALSE(lob::execution::FillModel(best_quote_config)
                   .check_fill(buy, depth_update_event(1200), make_book())
                   .has_value());

  lob::execution::FillModelConfig mid_config;
  mid_config.fill_reference = lob::execution::FillReference::MidPrice;
  mid_config.default_role = lob::execution::LiquidityRole::Taker;
  mid_config.taker_bps = 10.0;
  const auto fill =
      lob::execution::FillModel(mid_config).check_fill(buy, depth_update_event(1200), make_book());
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->liquidity_role, lob::execution::LiquidityRole::Taker);
  EXPECT_NEAR(fill->fee, 0.2, 1e-12);

  mid_config.taker_bps = std::numeric_limits<double>::infinity();
  EXPECT_THROW(static_cast<void>(lob::execution::FillModel{mid_config}), std::runtime_error);
}

TEST(FillModelTest, AllowsMakerRebatesAsNegativeFees) {
  lob::execution::OrderManager orders;
  const auto &buy = orders.submit_limit(1, lob::execution::OrderSide::Buy, 100, 2, 1000);

  auto config = trade_price_config();
  config.maker_bps = -2.5;
  const auto fill =
      lob::execution::FillModel(config).check_fill(buy, trade_event(1100, 99), make_book());

  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->liquidity_role, lob::execution::LiquidityRole::Maker);
  EXPECT_NEAR(fill->fee, -0.05, 1e-12);
}

TEST(FillModelIntegrationTest, ActiveOrdersFillOnTradesAndRoundTripPnlIsExactWithZeroFees) {
  lob::execution::OrderManager orders;
  const auto &buy = orders.submit_limit(1, lob::execution::OrderSide::Buy, 99, 2, 1000);
  const auto &sell = orders.submit_limit(1, lob::execution::OrderSide::Sell, 101, 2, 1001);
  ASSERT_EQ(orders.active_count(), 2U);

  const lob::execution::FillModel model(trade_price_config());
  const auto book = make_book();
  std::vector<lob::execution::Fill> fills;

  auto first_fills = model.fill_active_orders(orders, trade_event(1100, 99), book);
  fills.insert(fills.end(), first_fills.begin(), first_fills.end());
  EXPECT_EQ(orders.find_order(buy.id)->status, lob::execution::OrderStatus::Filled);
  EXPECT_EQ(orders.find_order(sell.id)->status, lob::execution::OrderStatus::Active);

  auto second_fills = model.fill_active_orders(orders, trade_event(1200, 101), book);
  fills.insert(fills.end(), second_fills.begin(), second_fills.end());
  EXPECT_EQ(orders.find_order(sell.id)->status, lob::execution::OrderStatus::Filled);
  EXPECT_EQ(orders.active_count(), 0U);

  lob::execution::Quantity inventory = 0;
  double cash = 0.0;
  for (const lob::execution::Fill &fill : fills) {
    const auto signed_quantity =
        fill.side == lob::execution::OrderSide::Buy ? fill.quantity_lots : -fill.quantity_lots;
    inventory += signed_quantity;
    cash -= static_cast<double>(signed_quantity * fill.fill_price_ticks);
    cash -= fill.fee;
  }

  ASSERT_EQ(fills.size(), 2U);
  EXPECT_EQ(inventory, 0);
  EXPECT_EQ(cash, static_cast<double>((101 - 99) * 2));
}
