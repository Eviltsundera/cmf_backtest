#include "lob/data/CsvDataSource.hpp"
#include "lob/engine/BacktestEngine.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

class VectorDataSource final : public lob::data::IDataSource {
public:
  explicit VectorDataSource(std::vector<lob::data::MarketEvent> events)
      : events_(std::move(events)) {
  }

  bool next(lob::data::MarketEvent &event) override {
    if (next_index_ >= events_.size()) {
      return false;
    }
    event = events_[next_index_++];
    return true;
  }

private:
  std::vector<lob::data::MarketEvent> events_;
  std::size_t next_index_ = 0;
};

lob::data::MarketEvent snapshot_event(const std::int64_t ts_ns, const std::uint64_t seq,
                                      const lob::book::Price bid_price,
                                      const lob::book::Quantity bid_qty,
                                      const lob::book::Price ask_price,
                                      const lob::book::Quantity ask_qty) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.seq = seq;
  event.type = lob::data::EventType::Snapshot;
  event.payload.snapshot.depth = 1;
  event.payload.snapshot.bids[0] =
      lob::data::PriceLevel{.price_ticks = bid_price, .quantity_lots = bid_qty};
  event.payload.snapshot.asks[0] =
      lob::data::PriceLevel{.price_ticks = ask_price, .quantity_lots = ask_qty};
  return event;
}

lob::data::MarketEvent trade_event(const std::int64_t ts_ns, const std::uint64_t seq,
                                   const lob::data::TradeSide side, const lob::book::Price price) {
  lob::data::MarketEvent event{};
  event.ts_ns = ts_ns;
  event.seq = seq;
  event.type = lob::data::EventType::Trade;
  event.payload.trade =
      lob::data::TradePayload{.side = side, .price_ticks = price, .quantity_lots = 1};
  return event;
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

class FixedQuoteStrategy final : public lob::strategies::IStrategy {
public:
  std::vector<lob::execution::OrderIntent>
  on_market_event(const lob::data::MarketEvent &event,
                  const lob::strategies::MarketState &) override {
    if (quoted_) {
      return {};
    }
    quoted_ = true;
    return {
        lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Buy, 99, 1,
                                                  event.ts_ns),
        lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Sell, 101, 1,
                                                  event.ts_ns),
    };
  }

  std::vector<lob::execution::OrderIntent> on_fill(const lob::execution::Fill &fill,
                                                   const lob::strategies::MarketState &) override {
    fills.push_back(fill);
    return {};
  }

  std::vector<lob::execution::Fill> fills;

private:
  bool quoted_ = false;
};

class RecordingStrategy final : public lob::strategies::IStrategy {
public:
  std::vector<lob::execution::OrderIntent>
  on_market_event(const lob::data::MarketEvent &event,
                  const lob::strategies::MarketState &state) override {
    event_sequences.push_back(event.seq);
    mids.push_back(state.mid_price.value_or(-1.0));
    return {};
  }

  std::vector<lob::execution::OrderIntent> on_fill(const lob::execution::Fill &,
                                                   const lob::strategies::MarketState &) override {
    return {};
  }

  std::vector<std::uint64_t> event_sequences;
  std::vector<double> mids;
};

class ReplacingAfterBatchFillStrategy final : public lob::strategies::IStrategy {
public:
  std::vector<lob::execution::OrderIntent>
  on_market_event(const lob::data::MarketEvent &event,
                  const lob::strategies::MarketState &) override {
    if (quoted_) {
      return {};
    }
    quoted_ = true;
    return {
        lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Buy, 99, 1,
                                                  event.ts_ns),
        lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Buy, 99, 1,
                                                  event.ts_ns),
    };
  }

  std::vector<lob::execution::OrderIntent>
  on_fill(const lob::execution::Fill &, const lob::strategies::MarketState &state) override {
    fill_callback_inventories.push_back(state.inventory_lots);
    return {lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Buy, 99, 1,
                                                      state.ts_ns)};
  }

  std::vector<lob::execution::Quantity> fill_callback_inventories;

private:
  bool quoted_ = false;
};

class ReplaceOnceStrategy final : public lob::strategies::IStrategy {
public:
  std::vector<lob::execution::OrderIntent>
  on_market_event(const lob::data::MarketEvent &event,
                  const lob::strategies::MarketState &) override {
    if (event.seq == 1) {
      return {lob::execution::OrderIntent::submit_limit(1, lob::execution::OrderSide::Buy, 98, 1,
                                                        event.ts_ns)};
    }
    if (event.seq == 2) {
      return {lob::execution::OrderIntent::replace(1, 99, 1, event.ts_ns)};
    }
    return {};
  }

  std::vector<lob::execution::OrderIntent> on_fill(const lob::execution::Fill &,
                                                   const lob::strategies::MarketState &) override {
    return {};
  }
};

lob::engine::BacktestEngineConfig default_engine_config() {
  lob::engine::BacktestEngineConfig config;
  config.book.max_depth = 10;
  config.fills.fill_reference = lob::execution::FillReference::TradePrice;
  config.quote_refresh_ns = 1'000'000;
  return config;
}

} // namespace

TEST(BacktestEngineTest, RunsSyntheticStreamThroughOrdersFillsPortfolioMetricsAndArtifacts) {
  VectorDataSource source({
      snapshot_event(1'000, 1, 99, 10, 101, 10),
      trade_event(2'000, 2, lob::data::TradeSide::Sell, 99),
      trade_event(3'000, 3, lob::data::TradeSide::Buy, 101),
  });
  FixedQuoteStrategy strategy;

  auto config = default_engine_config();
  const auto output_dir = make_temp_dir("lob_engine_synthetic");
  config.output_dir = output_dir;

  const lob::engine::BacktestResult result =
      lob::engine::BacktestEngine(config).run(source, strategy);

  EXPECT_EQ(result.event_counts.total(), 3U);
  EXPECT_EQ(result.fill_count, 2U);
  EXPECT_EQ(strategy.fills.size(), 2U);
  EXPECT_EQ(result.portfolio.position_lots(), 0);
  EXPECT_DOUBLE_EQ(result.portfolio.cash(), 2.0);
  EXPECT_DOUBLE_EQ(result.metrics.final_pnl, 2.0);
  EXPECT_EQ(result.metrics.fill_count, 2U);
  EXPECT_DOUBLE_EQ(result.metrics.fill_rate, 1.0);
  EXPECT_EQ(result.active_order_count, 0U);

  EXPECT_NE(read_file(output_dir / "metrics.json").find("\"final_pnl\""), std::string::npos);
  EXPECT_EQ(read_file(output_dir / "fills.csv")
                .find("ts_ns,order_id,strategy_id,side,limit_price_ticks,fill_price_ticks,"
                      "quantity_lots,liquidity_role,fee"),
            0U);
  EXPECT_EQ(read_file(output_dir / "orders.csv")
                .find("ts_ns,event_type,order_id,strategy_id,side,price_ticks"),
            0U);
  EXPECT_EQ(read_file(output_dir / "equity_curve.csv")
                .find("ts_ns,mark_price,equity,total_pnl,realized_pnl,unrealized_pnl,cash,"
                      "position_lots"),
            0U);
  EXPECT_EQ(read_file(output_dir / "inventory.csv").find("ts_ns,position_lots"), 0U);
  std::filesystem::remove_all(output_dir);
}

TEST(BacktestEngineTest, AppliesSameEventFillBatchBeforeFillCallbacks) {
  VectorDataSource source({
      snapshot_event(1'000, 1, 99, 10, 101, 10),
      trade_event(2'000, 2, lob::data::TradeSide::Sell, 99),
  });
  ReplacingAfterBatchFillStrategy strategy;

  auto config = default_engine_config();
  config.orders.risk.max_inventory_lots = 2;
  config.quote_refresh_ns = 0;

  const lob::engine::BacktestResult result =
      lob::engine::BacktestEngine(config).run(source, strategy);

  EXPECT_EQ(result.fill_count, 2U);
  EXPECT_EQ(result.portfolio.position_lots(), 2);
  EXPECT_EQ(result.active_order_count, 0U);
  ASSERT_EQ(strategy.fill_callback_inventories.size(), 2U);
  EXPECT_EQ(strategy.fill_callback_inventories[0], 2);
  EXPECT_EQ(strategy.fill_callback_inventories[1], 2);
}

TEST(BacktestEngineTest, CountsAcceptedReplaceAsFillOpportunity) {
  VectorDataSource source({
      snapshot_event(1'000, 1, 99, 10, 101, 10),
      snapshot_event(2'000, 2, 99, 10, 101, 10),
      trade_event(3'000, 3, lob::data::TradeSide::Sell, 99),
  });
  ReplaceOnceStrategy strategy;

  auto config = default_engine_config();
  config.quote_refresh_ns = 0;

  const lob::engine::BacktestResult result =
      lob::engine::BacktestEngine(config).run(source, strategy);

  EXPECT_EQ(result.fill_count, 1U);
  EXPECT_EQ(result.metrics.fill_count, 1U);
  EXPECT_DOUBLE_EQ(result.metrics.fill_rate, 0.5);
}

TEST(BacktestEngineTest, StrategySeesOnlyCurrentAppliedMarketState) {
  VectorDataSource source({
      snapshot_event(1'000, 10, 99, 10, 101, 10),
      snapshot_event(2'000, 11, 103, 10, 105, 10),
  });
  RecordingStrategy strategy;

  auto config = default_engine_config();
  config.quote_refresh_ns = 0;
  const lob::engine::BacktestResult result =
      lob::engine::BacktestEngine(config).run(source, strategy);

  EXPECT_EQ(result.event_counts.snapshots, 2U);
  ASSERT_EQ(strategy.event_sequences.size(), 2U);
  ASSERT_EQ(strategy.mids.size(), 2U);
  EXPECT_EQ(strategy.event_sequences[0], 10U);
  EXPECT_EQ(strategy.event_sequences[1], 11U);
  EXPECT_DOUBLE_EQ(strategy.mids[0], 100.0);
  EXPECT_DOUBLE_EQ(strategy.mids[1], 104.0);
}

TEST(BacktestEngineIntegrationTest, ReplaysSampleAndReportsThroughput) {
  const auto data_dir = std::filesystem::path(LOB_TEST_DATA_DIR);
  ASSERT_TRUE(std::filesystem::exists(data_dir / "lob.csv"));
  ASSERT_TRUE(std::filesystem::exists(data_dir / "trades.csv"));

  lob::data::CsvDataSource source(lob::data::csv_config_from_directory(data_dir, 0.0000001, 1.0));
  lob::strategies::NoopStrategy strategy;
  auto config = default_engine_config();
  config.book.max_depth = 50;
  config.quote_refresh_ns = 1'000'000'000;

  const lob::engine::BacktestResult result =
      lob::engine::BacktestEngine(config).run(source, strategy);

  EXPECT_EQ(result.event_counts.snapshots, 7200U);
  EXPECT_EQ(result.event_counts.depth_updates, 0U);
  EXPECT_EQ(result.event_counts.trades, 750467U);
  EXPECT_EQ(result.event_counts.total(), 757667U);
  EXPECT_GT(result.elapsed_seconds, 0.0);
  EXPECT_GT(result.events_per_second, 0.0);
  EXPECT_EQ(result.fill_count, 0U);

  std::cout << "engine_sample_events_per_sec=" << std::fixed << std::setprecision(0)
            << result.events_per_second << '\n';
}
