#include "lob/book/OrderBook.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>

#include <gtest/gtest.h>

namespace {

lob::data::SnapshotPayload make_snapshot() {
  lob::data::SnapshotPayload snapshot{};
  snapshot.depth = 2;
  snapshot.asks[0] = lob::data::PriceLevel{.price_ticks = 105, .quantity_lots = 10};
  snapshot.asks[1] = lob::data::PriceLevel{.price_ticks = 110, .quantity_lots = 5};
  snapshot.bids[0] = lob::data::PriceLevel{.price_ticks = 100, .quantity_lots = 8};
  snapshot.bids[1] = lob::data::PriceLevel{.price_ticks = 95, .quantity_lots = 4};
  return snapshot;
}

lob::book::OrderBookConfig config_with_depth(const std::uint32_t max_depth) {
  lob::book::OrderBookConfig config;
  config.max_depth = max_depth;
  config.crossed_book_policy = lob::book::CrossedBookPolicy::DropCrossingLevels;
  return config;
}

void expect_not_crossed(const lob::book::OrderBook &book) {
  EXPECT_FALSE(book.is_crossed_or_locked());
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (bid && ask) {
    EXPECT_LT(bid->price_ticks, ask->price_ticks);
  }
}

} // namespace

TEST(OrderBookTest, ApplySnapshotRestoresBookFromEmpty) {
  lob::book::OrderBook book(config_with_depth(10));
  ASSERT_TRUE(book.empty());

  book.apply_snapshot(make_snapshot());

  ASSERT_EQ(book.snapshot_id(), 1U);
  ASSERT_EQ(book.bid_depth(), 2U);
  ASSERT_EQ(book.ask_depth(), 2U);
  ASSERT_TRUE(book.best_bid().has_value());
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_bid()->price_ticks, 100);
  EXPECT_EQ(book.best_bid()->quantity_lots, 8);
  EXPECT_EQ(book.best_ask()->price_ticks, 105);
  EXPECT_EQ(book.best_ask()->quantity_lots, 10);
  EXPECT_EQ(book.mid(), 102.5);
  EXPECT_EQ(book.spread(), 5);
  ASSERT_TRUE(book.level(lob::data::BookSide::Bid, 1).has_value());
  EXPECT_EQ(book.level(lob::data::BookSide::Bid, 1)->price_ticks, 95);
  ASSERT_TRUE(book.level(lob::data::BookSide::Ask, 1).has_value());
  EXPECT_EQ(book.level(lob::data::BookSide::Ask, 1)->price_ticks, 110);
}

TEST(OrderBookTest, ApplySnapshotClearsOldLevelsAndRespectsMaxDepth) {
  lob::book::OrderBook book(config_with_depth(1));
  book.apply_update(lob::data::BookSide::Bid, 90, 3);
  book.apply_update(lob::data::BookSide::Ask, 120, 2);

  book.apply_snapshot(make_snapshot());

  EXPECT_EQ(book.snapshot_id(), 1U);
  EXPECT_EQ(book.bid_depth(), 1U);
  EXPECT_EQ(book.ask_depth(), 1U);
  EXPECT_EQ(book.best_bid()->price_ticks, 100);
  EXPECT_EQ(book.best_ask()->price_ticks, 105);
  EXPECT_FALSE(book.level(lob::data::BookSide::Bid, 1).has_value());
  EXPECT_FALSE(book.level(lob::data::BookSide::Ask, 1).has_value());
}

TEST(OrderBookTest, ApplyUpdateAddsUpdatesAndDeletesLevels) {
  lob::book::OrderBook book(config_with_depth(10));

  book.apply_update(lob::data::BookSide::Bid, 100, 5);
  book.apply_update(lob::data::BookSide::Ask, 105, 7);
  EXPECT_EQ(book.best_bid()->quantity_lots, 5);
  EXPECT_EQ(book.best_ask()->quantity_lots, 7);

  book.apply_update(lob::data::BookSide::Bid, 100, 9);
  EXPECT_EQ(book.best_bid()->quantity_lots, 9);

  book.apply_update(lob::data::BookSide::Bid, 100, 0);
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_TRUE(book.best_ask().has_value());
}

TEST(OrderBookTest, ApplyEventBuildsBookAndIgnoresTrades) {
  lob::book::OrderBook book(config_with_depth(10));

  lob::data::MarketEvent snapshot_event{};
  snapshot_event.type = lob::data::EventType::Snapshot;
  snapshot_event.payload.snapshot = make_snapshot();
  EXPECT_TRUE(book.apply_event(snapshot_event));

  lob::data::MarketEvent trade_event{};
  trade_event.type = lob::data::EventType::Trade;
  trade_event.payload.trade = lob::data::TradePayload{
      .side = lob::data::TradeSide::Buy, .price_ticks = 105, .quantity_lots = 1};
  EXPECT_FALSE(book.apply_event(trade_event));
  EXPECT_EQ(book.best_bid()->price_ticks, 100);
  EXPECT_EQ(book.best_ask()->price_ticks, 105);
}

TEST(OrderBookTest, RecoveryDropsCrossingUpdatedLevels) {
  lob::book::OrderBook book(config_with_depth(10));
  book.apply_update(lob::data::BookSide::Bid, 99, 3);
  book.apply_update(lob::data::BookSide::Ask, 100, 4);

  book.apply_update(lob::data::BookSide::Bid, 101, 5);
  expect_not_crossed(book);
  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(book.best_bid()->price_ticks, 99);

  book.apply_update(lob::data::BookSide::Ask, 98, 5);
  expect_not_crossed(book);
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(book.best_ask()->price_ticks, 100);
}

TEST(OrderBookTest, RejectPolicyDetectsCrossedBookAndRollsBackUpdate) {
  lob::book::OrderBookConfig config;
  config.max_depth = 10;
  config.crossed_book_policy = lob::book::CrossedBookPolicy::Reject;
  lob::book::OrderBook book(config);
  book.apply_update(lob::data::BookSide::Bid, 99, 3);
  book.apply_update(lob::data::BookSide::Ask, 100, 4);

  EXPECT_THROW(book.apply_update(lob::data::BookSide::Bid, 101, 1), std::runtime_error);
  expect_not_crossed(book);
  EXPECT_EQ(book.best_bid()->price_ticks, 99);
}

TEST(OrderBookPropertyTest, RandomUpdatesKeepInvariantWithRecovery) {
  lob::book::OrderBook book(config_with_depth(20));
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<int> price_dist(95, 105);
  std::uniform_int_distribution<int> quantity_dist(0, 20);

  for (int price = 90; price < 100; ++price) {
    book.apply_update(lob::data::BookSide::Bid, price, 1);
  }
  for (int price = 101; price < 111; ++price) {
    book.apply_update(lob::data::BookSide::Ask, price, 1);
  }

  for (int index = 0; index < 5000; ++index) {
    const auto side = side_dist(rng) == 0 ? lob::data::BookSide::Bid : lob::data::BookSide::Ask;
    const auto price = static_cast<std::int64_t>(price_dist(rng));
    const auto quantity = static_cast<std::int64_t>(quantity_dist(rng));
    EXPECT_NO_THROW(book.apply_update(side, price, quantity));
    expect_not_crossed(book);
    EXPECT_LE(book.bid_depth(), 20U);
    EXPECT_LE(book.ask_depth(), 20U);
  }
}

TEST(OrderBookBenchmarkTest, ApplyUpdateExceedsOneMillionOpsPerSecondInRelease) {
#ifndef NDEBUG
  GTEST_SKIP() << "Performance benchmark requires a Release build";
#else
  lob::book::OrderBook book(config_with_depth(100));
  for (int offset = 0; offset < 100; ++offset) {
    book.apply_update(lob::data::BookSide::Bid, 9999 - offset, 1);
    book.apply_update(lob::data::BookSide::Ask, 10001 + offset, 1);
  }

  constexpr std::size_t kOps = 250000;
  std::int64_t checksum = 0;
  const auto started_at = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < kOps; ++index) {
    const auto quantity = static_cast<std::int64_t>((index % 100) + 1);
    if (index % 2 == 0) {
      book.apply_update(lob::data::BookSide::Bid, 9950 - static_cast<std::int64_t>(index % 50),
                        quantity);
    } else {
      book.apply_update(lob::data::BookSide::Ask, 10050 + static_cast<std::int64_t>(index % 50),
                        quantity);
    }
    checksum += book.best_bid()->quantity_lots;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
  const double ops_per_second = static_cast<double>(kOps) / elapsed_seconds;

  std::cout << "order_book_apply_update_ops_per_sec=" << static_cast<std::int64_t>(ops_per_second)
            << '\n';
  EXPECT_GT(checksum, 0);
  EXPECT_GE(ops_per_second, 1000000.0);
  expect_not_crossed(book);
#endif
}
