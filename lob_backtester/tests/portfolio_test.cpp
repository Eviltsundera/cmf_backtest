#include "lob/portfolio/Portfolio.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

lob::execution::Fill make_fill(const lob::execution::OrderSide side,
                               const lob::execution::Price price_ticks,
                               const lob::execution::Quantity quantity_lots,
                               const double fee = 0.0) {
  lob::execution::Fill fill;
  fill.side = side;
  fill.fill_price_ticks = price_ticks;
  fill.limit_price_ticks = price_ticks;
  fill.quantity_lots = quantity_lots;
  fill.fee = fee;
  return fill;
}

} // namespace

TEST(PortfolioTest, AppliesBuySellFeesAndMarkToMarket) {
  lob::portfolio::Portfolio portfolio;

  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 100, 2, 1.0));
  EXPECT_EQ(portfolio.position_lots(), 2);
  EXPECT_DOUBLE_EQ(portfolio.cash(), -201.0);
  EXPECT_DOUBLE_EQ(portfolio.average_entry_price(), 100.0);
  EXPECT_DOUBLE_EQ(portfolio.realized_pnl(), -1.0);
  EXPECT_DOUBLE_EQ(portfolio.unrealized_pnl(105.0), 10.0);
  EXPECT_DOUBLE_EQ(portfolio.equity(105.0), 9.0);

  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Sell, 110, 1, 0.5));
  EXPECT_EQ(portfolio.position_lots(), 1);
  EXPECT_DOUBLE_EQ(portfolio.cash(), -91.5);
  EXPECT_DOUBLE_EQ(portfolio.average_entry_price(), 100.0);
  EXPECT_DOUBLE_EQ(portfolio.realized_pnl(), 8.5);
  EXPECT_DOUBLE_EQ(portfolio.unrealized_pnl(105.0), 5.0);
  EXPECT_DOUBLE_EQ(portfolio.total_pnl(105.0), 13.5);
}

TEST(PortfolioTest, RoundTripBySpreadMatchesSpreadTimesQuantityWithoutFees) {
  lob::portfolio::Portfolio portfolio;

  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 99, 3));
  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Sell, 101, 3));

  EXPECT_EQ(portfolio.position_lots(), 0);
  EXPECT_DOUBLE_EQ(portfolio.cash(), static_cast<double>((101 - 99) * 3));
  EXPECT_DOUBLE_EQ(portfolio.realized_pnl(), static_cast<double>((101 - 99) * 3));
  EXPECT_DOUBLE_EQ(portfolio.unrealized_pnl(100.0), 0.0);
  EXPECT_DOUBLE_EQ(portfolio.total_pnl(100.0), static_cast<double>((101 - 99) * 3));
}

TEST(PortfolioTest, SupportsShortPositionsAndReversals) {
  lob::portfolio::Portfolio portfolio;

  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Sell, 100, 5));
  EXPECT_EQ(portfolio.position_lots(), -5);
  EXPECT_DOUBLE_EQ(portfolio.cash(), 500.0);
  EXPECT_DOUBLE_EQ(portfolio.average_entry_price(), 100.0);
  EXPECT_DOUBLE_EQ(portfolio.unrealized_pnl(95.0), 25.0);

  portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 90, 8));
  EXPECT_EQ(portfolio.position_lots(), 3);
  EXPECT_DOUBLE_EQ(portfolio.cash(), -220.0);
  EXPECT_DOUBLE_EQ(portfolio.average_entry_price(), 90.0);
  EXPECT_DOUBLE_EQ(portfolio.realized_pnl(), 50.0);
  EXPECT_DOUBLE_EQ(portfolio.total_pnl(95.0), 65.0);
}

TEST(PortfolioTest, RejectsInvalidInputs) {
  EXPECT_THROW(
      static_cast<void>(lob::portfolio::Portfolio{std::numeric_limits<double>::quiet_NaN()}),
      std::runtime_error);

  lob::portfolio::Portfolio portfolio;
  EXPECT_THROW(portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 0, 1)),
               std::runtime_error);
  EXPECT_THROW(portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 100, 0)),
               std::runtime_error);
  EXPECT_THROW(portfolio.apply_fill(make_fill(lob::execution::OrderSide::Buy, 100, 1,
                                              std::numeric_limits<double>::infinity())),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(portfolio.equity(0.0)), std::runtime_error);
}
