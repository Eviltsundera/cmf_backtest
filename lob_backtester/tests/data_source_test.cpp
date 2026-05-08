#include "lob/data/CsvDataSource.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

namespace {

constexpr double kTickSize = 0.0000001;
constexpr double kLotSize = 1.0;

std::filesystem::path make_temp_dir(const std::string &name) {
  const auto suffix =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto path =
      std::filesystem::temp_directory_path() / (name + "_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

void write_file(const std::filesystem::path &path, const std::string &content) {
  std::ofstream out(path);
  ASSERT_TRUE(out) << path;
  out << content;
}

std::string snapshot_header(const int depth) {
  std::string header = ",local_timestamp";
  for (int level = 0; level < depth; ++level) {
    header += ",asks[" + std::to_string(level) + "].price";
    header += ",asks[" + std::to_string(level) + "].amount";
    header += ",bids[" + std::to_string(level) + "].price";
    header += ",bids[" + std::to_string(level) + "].amount";
  }
  header += '\n';
  return header;
}

lob::data::CsvDataSourceConfig one_source_config() {
  lob::data::CsvDataSourceConfig config;
  config.tick_size = kTickSize;
  config.lot_size = kLotSize;
  config.snapshot_depth = 2;
  return config;
}

} // namespace

TEST(CsvDataSourceTest, ParsesSnapshotRows) {
  const auto dir = make_temp_dir("lob_snapshot");
  const auto path = dir / "lob.csv";
  write_file(path, snapshot_header(2) +
                       "0,1000,0.0000003,2.0,0.0000002,3.0,"
                       "0.0000004,5.0,0.0000001,7.0\n");

  auto config = one_source_config();
  config.snapshots_path = path;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.ts_ns, 1000000);
  EXPECT_EQ(event.seq, 0U);
  EXPECT_EQ(event.type, lob::data::EventType::Snapshot);
  EXPECT_EQ(event.payload.snapshot.depth, 2);
  EXPECT_EQ(event.payload.snapshot.asks[0].price_ticks, 3);
  EXPECT_EQ(event.payload.snapshot.asks[0].quantity_lots, 2);
  EXPECT_EQ(event.payload.snapshot.bids[1].price_ticks, 1);
  EXPECT_EQ(event.payload.snapshot.bids[1].quantity_lots, 7);
  EXPECT_FALSE(source.next(event));

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, ParsesTradeRows) {
  const auto dir = make_temp_dir("lob_trade");
  const auto path = dir / "trades.csv";
  write_file(path, ",local_timestamp,side,price,amount\n"
                   "7,1001,buy,0.0000003,4\n");

  auto config = one_source_config();
  config.trades_path = path;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.ts_ns, 1001000);
  EXPECT_EQ(event.type, lob::data::EventType::Trade);
  EXPECT_EQ(event.payload.trade.side, lob::data::TradeSide::Buy);
  EXPECT_EQ(event.payload.trade.price_ticks, 3);
  EXPECT_EQ(event.payload.trade.quantity_lots, 4);
  EXPECT_FALSE(source.next(event));

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, ParsesDepthUpdateRows) {
  const auto dir = make_temp_dir("lob_depth_update");
  const auto path = dir / "depth_updates.csv";
  write_file(path, ",local_timestamp,side,price,amount\n"
                   "3,999,bid,0.0000002,5\n");

  auto config = one_source_config();
  config.depth_updates_path = path;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.ts_ns, 999000);
  EXPECT_EQ(event.type, lob::data::EventType::DepthUpdate);
  EXPECT_EQ(event.payload.depth_update.side, lob::data::BookSide::Bid);
  EXPECT_EQ(event.payload.depth_update.price_ticks, 2);
  EXPECT_EQ(event.payload.depth_update.quantity_lots, 5);
  EXPECT_FALSE(source.next(event));

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, DetectsTimestampRegressionInsideSource) {
  const auto dir = make_temp_dir("lob_regression");
  const auto path = dir / "trades.csv";
  write_file(path, ",local_timestamp,side,price,amount\n"
                   "0,1001,buy,0.0000003,4\n"
                   "1,1000,buy,0.0000003,4\n");

  auto config = one_source_config();
  config.trades_path = path;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_THROW(source.next(event), std::runtime_error);

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, RejectsDuplicateEventKeys) {
  const auto dir = make_temp_dir("lob_duplicate");
  const auto path = dir / "trades.csv";
  write_file(path, ",local_timestamp,side,price,amount\n"
                   "0,1000,buy,0.0000003,4\n"
                   "0,1000,buy,0.0000003,5\n");

  auto config = one_source_config();
  config.trades_path = path;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_THROW(source.next(event), std::runtime_error);

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, RejectsTickMisalignment) {
  const auto dir = make_temp_dir("lob_tick_misalignment");
  const auto path = dir / "trades.csv";
  write_file(path, ",local_timestamp,side,price,amount\n"
                   "0,1000,buy,0.00000015,4\n");

  auto config = one_source_config();
  config.trades_path = path;
  EXPECT_THROW(lob::data::CsvDataSource source(config), std::runtime_error);

  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceTest, MergesSourcesByTimestampAndSequence) {
  const auto dir = make_temp_dir("lob_merge");
  const auto snapshots = dir / "lob.csv";
  const auto trades = dir / "trades.csv";
  write_file(snapshots, snapshot_header(1) +
                            "10,1000,0.0000003,2.0,0.0000002,3.0\n");
  write_file(trades, ",local_timestamp,side,price,amount\n"
                     "1,900,sell,0.0000002,1\n"
                     "2,1000,buy,0.0000003,1\n");

  auto config = one_source_config();
  config.snapshot_depth = 1;
  config.snapshots_path = snapshots;
  config.trades_path = trades;
  lob::data::CsvDataSource source(config);

  lob::data::MarketEvent event{};
  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.type, lob::data::EventType::Trade);
  EXPECT_EQ(event.ts_ns, 900000);

  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.type, lob::data::EventType::Snapshot);
  EXPECT_EQ(event.ts_ns, 1000000);

  ASSERT_TRUE(source.next(event));
  EXPECT_EQ(event.type, lob::data::EventType::Trade);
  EXPECT_EQ(event.ts_ns, 1000000);

  EXPECT_FALSE(source.next(event));
  std::filesystem::remove_all(dir);
}

TEST(CsvDataSourceIntegrationTest, DrainsSampleWithAuditCounts) {
  const auto data_dir = std::filesystem::path(LOB_TEST_DATA_DIR);
  ASSERT_TRUE(std::filesystem::exists(data_dir / "lob.csv"));
  ASSERT_TRUE(std::filesystem::exists(data_dir / "trades.csv"));

  lob::data::CsvDataSource source(
      lob::data::csv_config_from_directory(data_dir, kTickSize, kLotSize));

  const auto started_at = std::chrono::steady_clock::now();
  const lob::data::EventCounts counts = lob::data::drain_data_source(source);
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  const double elapsed_seconds =
      std::chrono::duration<double>(elapsed).count();
  const double events_per_sec =
      static_cast<double>(counts.total()) / elapsed_seconds;

  EXPECT_EQ(counts.snapshots, 7200U);
  EXPECT_EQ(counts.depth_updates, 0U);
  EXPECT_EQ(counts.trades, 750467U);
  EXPECT_EQ(counts.total(), 757667U);
  EXPECT_GT(events_per_sec, 0.0);

  std::cout << "sample_events_per_sec=" << std::fixed << std::setprecision(0)
            << events_per_sec << '\n';
}
