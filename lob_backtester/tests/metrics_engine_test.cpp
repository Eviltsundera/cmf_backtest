#include "lob/metrics/MetricsEngine.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

lob::execution::Fill make_fill(const lob::execution::OrderSide side,
                               const lob::execution::Price price_ticks,
                               const lob::execution::Quantity quantity_lots) {
  lob::execution::Fill fill;
  fill.side = side;
  fill.fill_price_ticks = price_ticks;
  fill.limit_price_ticks = price_ticks;
  fill.quantity_lots = quantity_lots;
  return fill;
}

std::filesystem::path make_temp_dir(const std::string &name) {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto path = std::filesystem::temp_directory_path() / (name + "_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(MetricsEngineTest, NoTradingRunHasZeroPnlTurnoverAndWritesOutputFields) {
  lob::portfolio::Portfolio portfolio;
  lob::metrics::MetricsEngine metrics;
  metrics.record_equity(1000, portfolio, 100.0);
  metrics.record_quote(1000, std::nullopt, std::nullopt);

  const auto snapshot = metrics.compute();
  EXPECT_DOUBLE_EQ(snapshot.final_pnl, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.mean_inventory, 0.0);
  EXPECT_EQ(snapshot.max_inventory, 0);
  EXPECT_DOUBLE_EQ(snapshot.inventory_std, 0.0);
  EXPECT_EQ(snapshot.turnover_qty, 0);
  EXPECT_DOUBLE_EQ(snapshot.turnover_notional, 0.0);
  EXPECT_EQ(snapshot.fill_count, 0U);
  EXPECT_DOUBLE_EQ(snapshot.fill_rate, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.max_drawdown, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.quote_uptime, 0.0);

  const auto dir = make_temp_dir("lob_metrics_no_trading");
  metrics.write_run_outputs(dir);

  const std::string json = read_file(dir / "metrics.json");
  EXPECT_NE(json.find("\"final_pnl\""), std::string::npos);
  EXPECT_NE(json.find("\"mean_inventory\""), std::string::npos);
  EXPECT_NE(json.find("\"max_inventory\""), std::string::npos);
  EXPECT_NE(json.find("\"inventory_std\""), std::string::npos);
  EXPECT_NE(json.find("\"turnover_qty\""), std::string::npos);
  EXPECT_NE(json.find("\"turnover_notional\""), std::string::npos);
  EXPECT_NE(json.find("\"fill_count\""), std::string::npos);
  EXPECT_NE(json.find("\"fill_rate\""), std::string::npos);
  EXPECT_NE(json.find("\"max_drawdown\""), std::string::npos);
  EXPECT_NE(json.find("\"avg_quoted_spread\""), std::string::npos);
  EXPECT_NE(json.find("\"avg_spread_captured\""), std::string::npos);
  EXPECT_NE(json.find("\"adverse_selection_h\""), std::string::npos);
  EXPECT_NE(json.find("\"quote_uptime\""), std::string::npos);

  EXPECT_EQ(read_file(dir / "equity_curve.csv")
                .find("ts_ns,mark_price,equity,total_pnl,realized_pnl,unrealized_pnl,cash,"
                      "position_lots"),
            0U);
  EXPECT_EQ(read_file(dir / "inventory.csv").find("ts_ns,position_lots"), 0U);
  std::filesystem::remove_all(dir);
}

TEST(MetricsEngineTest, AggregatesInventoryTurnoverDrawdownAndMarketMakingMetrics) {
  lob::portfolio::Portfolio portfolio;
  lob::metrics::MetricsEngine metrics;
  metrics.set_fill_opportunity_count(4);

  metrics.record_equity(0, portfolio, 100.0);
  metrics.record_quote(0, std::nullopt, std::nullopt);

  const auto buy = make_fill(lob::execution::OrderSide::Buy, 99, 2);
  portfolio.apply_fill(buy);
  metrics.record_fill(buy, 100.0);
  metrics.record_adverse_selection(buy, 1000, 98.0);
  metrics.record_quote(1, 99.0, 101.0);
  metrics.record_equity(1, portfolio, 98.0);

  const auto sell = make_fill(lob::execution::OrderSide::Sell, 101, 2);
  portfolio.apply_fill(sell);
  metrics.record_fill(sell, 100.0);
  metrics.record_adverse_selection(sell, 1000, 99.0);
  metrics.record_quote(2, 98.0, 102.0);
  metrics.record_equity(2, portfolio, 100.0);

  const auto snapshot = metrics.compute();
  EXPECT_DOUBLE_EQ(snapshot.final_pnl, 4.0);
  EXPECT_NEAR(snapshot.mean_inventory, 2.0 / 3.0, 1e-12);
  EXPECT_EQ(snapshot.max_inventory, 2);
  EXPECT_NEAR(snapshot.inventory_std, std::sqrt(8.0 / 9.0), 1e-12);
  EXPECT_EQ(snapshot.turnover_qty, 4);
  EXPECT_DOUBLE_EQ(snapshot.turnover_notional, 400.0);
  EXPECT_EQ(snapshot.fill_count, 2U);
  EXPECT_DOUBLE_EQ(snapshot.fill_rate, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.max_drawdown, 2.0);
  EXPECT_DOUBLE_EQ(snapshot.avg_quoted_spread, 3.0);
  EXPECT_DOUBLE_EQ(snapshot.avg_spread_captured, 1.0);
  EXPECT_DOUBLE_EQ(snapshot.quote_uptime, 2.0 / 3.0);
  ASSERT_TRUE(snapshot.adverse_selection_h.contains(1000));
  EXPECT_DOUBLE_EQ(snapshot.adverse_selection_h.at(1000), 0.5);
}

TEST(MetricsEngineTest, RejectsInvalidQuotesAndAdverseSelectionInputs) {
  lob::metrics::MetricsEngine metrics;
  EXPECT_THROW(metrics.record_quote(0, 100.0, 100.0), std::runtime_error);
  metrics.record_quote(1, 99.0, 101.0);
  EXPECT_DOUBLE_EQ(metrics.compute().quote_uptime, 1.0);

  EXPECT_THROW(
      metrics.record_adverse_selection(make_fill(lob::execution::OrderSide::Buy, 99, 1), 0, 100.0),
      std::runtime_error);
  EXPECT_THROW(
      metrics.record_adverse_selection(make_fill(lob::execution::OrderSide::Buy, 99, 1), 1000, 0.0),
      std::runtime_error);
  EXPECT_TRUE(metrics.compute().adverse_selection_h.empty());
}
