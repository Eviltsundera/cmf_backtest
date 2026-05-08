#include "lob/execution/OrderManager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

lob::execution::OrderManagerConfig risk_config() {
  lob::execution::OrderManagerConfig config;
  config.risk.max_inventory_lots = 10;
  config.risk.min_quantity_lots = 1;
  config.risk.price_tick_multiple = 1;
  config.risk.strict_maker = false;
  return config;
}

lob::book::OrderBook make_book() {
  lob::book::OrderBook book;
  book.apply_update(lob::data::BookSide::Bid, 99, 5);
  book.apply_update(lob::data::BookSide::Ask, 101, 5);
  return book;
}

std::filesystem::path make_temp_file(const std::string &name) {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  return std::filesystem::temp_directory_path() / (name + "_" + suffix + ".csv");
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(OrderManagerTest, SubmitAddsActiveOrder) {
  lob::execution::OrderManager manager(risk_config());

  const auto &order = manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 3, 1000);

  EXPECT_EQ(order.id, 1U);
  EXPECT_EQ(order.strategy_id, 7U);
  EXPECT_EQ(order.side, lob::execution::OrderSide::Buy);
  EXPECT_EQ(order.status, lob::execution::OrderStatus::Active);
  EXPECT_EQ(order.remaining_lots, 3);
  EXPECT_EQ(manager.active_count(), 1U);
  ASSERT_NE(manager.find_active_order(order.id), nullptr);
  EXPECT_EQ(manager.event_log().size(), 1U);
}

TEST(OrderManagerTest, CancelMovesOrderToCancelled) {
  lob::execution::OrderManager manager(risk_config());
  const auto &order = manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 3, 1000);

  const auto *cancelled = manager.cancel(order.id, 1100);

  ASSERT_NE(cancelled, nullptr);
  EXPECT_EQ(cancelled->status, lob::execution::OrderStatus::Cancelled);
  EXPECT_EQ(cancelled->remaining_lots, 0);
  EXPECT_EQ(manager.active_count(), 0U);
  EXPECT_EQ(manager.find_active_order(order.id), nullptr);
  ASSERT_NE(manager.find_order(order.id), nullptr);
  EXPECT_EQ(manager.find_order(order.id)->status, lob::execution::OrderStatus::Cancelled);
}

TEST(OrderManagerTest, RejectsWhenMaxInventoryWouldBeExceeded) {
  lob::execution::OrderManager manager(risk_config());

  const auto &order = manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 11, 1000);

  EXPECT_EQ(order.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(order.remaining_lots, 0);
  EXPECT_EQ(order.reject_reason, "max_inventory exceeded");
  EXPECT_EQ(manager.active_count(), 0U);
  EXPECT_EQ(manager.event_log().front().event_type,
            lob::execution::OrderLifecycleEventType::Rejected);
}

TEST(OrderManagerTest, RejectsWorstCaseSameSideActiveFillsWithoutNettingOppositeSide) {
  lob::execution::OrderManager manager(risk_config());

  const auto &buy = manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 6, 1000);
  const auto &sell = manager.submit_limit(7, lob::execution::OrderSide::Sell, 102, 6, 1001);
  ASSERT_EQ(buy.status, lob::execution::OrderStatus::Active);
  ASSERT_EQ(sell.status, lob::execution::OrderStatus::Active);

  const auto &new_buy = manager.submit_limit(7, lob::execution::OrderSide::Buy, 97, 10, 1002);

  EXPECT_EQ(new_buy.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(new_buy.reject_reason, "max_inventory exceeded");
  EXPECT_EQ(manager.active_count(), 2U);
}

TEST(OrderManagerTest, ReplaceCancelsOldOrderAndSubmitsNewId) {
  lob::execution::OrderManager manager(risk_config());
  const auto &old_order = manager.submit_limit(7, lob::execution::OrderSide::Sell, 102, 3, 1000);

  const auto *new_order = manager.replace(old_order.id, 103, 4, 1200);

  ASSERT_NE(new_order, nullptr);
  EXPECT_NE(new_order->id, old_order.id);
  EXPECT_EQ(new_order->status, lob::execution::OrderStatus::Active);
  EXPECT_EQ(new_order->replaces_order_id, old_order.id);
  ASSERT_NE(manager.find_order(old_order.id), nullptr);
  EXPECT_EQ(manager.find_order(old_order.id)->status, lob::execution::OrderStatus::Cancelled);
  EXPECT_EQ(manager.active_count(), 1U);
  EXPECT_EQ(manager.find_active_order(new_order->id), new_order);
}

TEST(OrderManagerTest, FillRemovesOrderFromActiveStore) {
  lob::execution::OrderManager manager(risk_config());
  const auto &order = manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 3, 1000);

  const auto *filled = manager.fill(order.id, 1300);

  ASSERT_NE(filled, nullptr);
  EXPECT_EQ(filled->status, lob::execution::OrderStatus::Filled);
  EXPECT_EQ(filled->remaining_lots, 0);
  EXPECT_EQ(manager.active_count(), 0U);
  EXPECT_EQ(manager.find_active_order(order.id), nullptr);
  ASSERT_NE(manager.find_order(order.id), nullptr);
  EXPECT_EQ(manager.find_order(order.id)->status, lob::execution::OrderStatus::Filled);
}

TEST(OrderManagerTest, RiskGatesRejectBadTicksMinQtyAndStrictMakerCrossing) {
  auto config = risk_config();
  config.risk.price_tick_multiple = 5;
  config.risk.strict_maker = true;
  lob::execution::OrderManager manager(config);
  const auto book = make_book();

  const auto &bad_tick =
      manager.submit_limit(7, lob::execution::OrderSide::Buy, 98, 1, 1000, &book);
  EXPECT_EQ(bad_tick.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(bad_tick.reject_reason, "price is not tick aligned");

  const auto &bad_qty = manager.submit_limit(7, lob::execution::OrderSide::Buy, 95, 0, 1001, &book);
  EXPECT_EQ(bad_qty.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(bad_qty.reject_reason, "quantity below min_qty");

  const auto &crossing =
      manager.submit_limit(7, lob::execution::OrderSide::Buy, 105, 1, 1002, &book);
  EXPECT_EQ(crossing.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(crossing.reject_reason, "strict_maker buy crosses best ask");
}

TEST(OrderManagerTest, StrictMakerRejectsWhenBookIsMissing) {
  auto config = risk_config();
  config.risk.strict_maker = true;
  lob::execution::OrderManager manager(config);

  const auto &order = manager.submit_limit(7, lob::execution::OrderSide::Buy, 100, 1, 1000);

  EXPECT_EQ(order.status, lob::execution::OrderStatus::Rejected);
  EXPECT_EQ(order.reject_reason, "strict_maker requires order book");
}

TEST(OrderManagerTest, ProcessIntentSupportsCancelAllAndWritesCsvLifecycleLog) {
  lob::execution::OrderManager manager(risk_config());
  const auto first = manager.process_intent(
      lob::execution::OrderIntent::submit_limit(7, lob::execution::OrderSide::Buy, 98, 3, 1000));
  const auto second = manager.process_intent(
      lob::execution::OrderIntent::submit_limit(7, lob::execution::OrderSide::Sell, 102, 2, 1001));
  ASSERT_TRUE(first.accepted);
  ASSERT_TRUE(second.accepted);
  EXPECT_EQ(manager.active_count(), 2U);

  const auto cancelled = manager.process_intent(lob::execution::OrderIntent::cancel_all(7, 1100));

  EXPECT_TRUE(cancelled.accepted);
  EXPECT_EQ(cancelled.order_ids.size(), 2U);
  EXPECT_EQ(manager.active_count(), 0U);

  const auto path = make_temp_file("orders");
  manager.write_order_log_csv(path);
  const std::string csv = read_file(path);
  EXPECT_NE(csv.find("ts_ns,event_type,order_id"), std::string::npos);
  EXPECT_NE(csv.find("submitted"), std::string::npos);
  EXPECT_NE(csv.find("cancelled"), std::string::npos);
  std::filesystem::remove(path);
}
