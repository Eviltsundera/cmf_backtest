#pragma once

#include "lob/book/OrderBook.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lob::execution {

using OrderId = std::uint64_t;
using StrategyId = std::uint32_t;
using Price = book::Price;
using Quantity = book::Quantity;

enum class OrderSide : std::uint8_t {
  Buy = 0,
  Sell = 1,
};

enum class OrderStatus : std::uint8_t {
  Active = 0,
  Filled = 1,
  Cancelled = 2,
  Rejected = 3,
};

enum class OrderIntentType : std::uint8_t {
  SubmitLimit = 0,
  Cancel = 1,
  CancelAll = 2,
  Replace = 3,
};

enum class OrderLifecycleEventType : std::uint8_t {
  Submitted = 0,
  Rejected = 1,
  Cancelled = 2,
  Filled = 3,
};

struct Order {
  OrderId id = 0;
  StrategyId strategy_id = 0;
  OrderSide side = OrderSide::Buy;
  Price price_ticks = 0;
  Quantity quantity_lots = 0;
  Quantity remaining_lots = 0;
  OrderStatus status = OrderStatus::Rejected;
  std::int64_t ts_submit_ns = 0;
  std::optional<OrderId> replaces_order_id;
  std::string reject_reason;
};

struct OrderIntent {
  OrderIntentType type = OrderIntentType::SubmitLimit;
  StrategyId strategy_id = 0;
  OrderId order_id = 0;
  OrderSide side = OrderSide::Buy;
  Price price_ticks = 0;
  Quantity quantity_lots = 0;
  std::int64_t ts_ns = 0;

  static OrderIntent submit_limit(StrategyId strategy_id, OrderSide side, Price price_ticks,
                                  Quantity quantity_lots, std::int64_t ts_ns);
  static OrderIntent cancel(OrderId order_id, std::int64_t ts_ns);
  static OrderIntent cancel_all(StrategyId strategy_id, std::int64_t ts_ns);
  static OrderIntent replace(OrderId order_id, Price price_ticks, Quantity quantity_lots,
                             std::int64_t ts_ns);
};

struct OrderIntentResult {
  std::vector<OrderId> order_ids;
  bool accepted = false;
  std::string reason;
};

struct OrderLifecycleEvent {
  std::int64_t ts_ns = 0;
  OrderLifecycleEventType event_type = OrderLifecycleEventType::Submitted;
  OrderId order_id = 0;
  StrategyId strategy_id = 0;
  OrderSide side = OrderSide::Buy;
  Price price_ticks = 0;
  Quantity quantity_lots = 0;
  Quantity remaining_lots = 0;
  OrderStatus status = OrderStatus::Active;
  std::optional<OrderId> related_order_id;
  std::string reason;
};

struct OrderRiskConfig {
  Quantity max_inventory_lots = 1'000'000'000;
  Quantity min_quantity_lots = 1;
  Price price_tick_multiple = 1;
  bool strict_maker = false;
};

struct OrderManagerConfig {
  OrderRiskConfig risk;
};

class OrderManager {
public:
  explicit OrderManager(OrderManagerConfig config = {});

  const Order &submit_limit(StrategyId strategy_id, OrderSide side, Price price_ticks,
                            Quantity quantity_lots, std::int64_t ts_ns,
                            const book::OrderBook *book = nullptr,
                            Quantity current_inventory_lots = 0,
                            std::optional<OrderId> replaces_order_id = std::nullopt);
  const Order *cancel(OrderId order_id, std::int64_t ts_ns);
  std::vector<OrderId> cancel_all(StrategyId strategy_id, std::int64_t ts_ns);
  const Order *replace(OrderId order_id, Price price_ticks, Quantity quantity_lots,
                       std::int64_t ts_ns, const book::OrderBook *book = nullptr,
                       Quantity current_inventory_lots = 0);
  const Order *fill(OrderId order_id, std::int64_t ts_ns);
  OrderIntentResult process_intent(const OrderIntent &intent, const book::OrderBook *book = nullptr,
                                   Quantity current_inventory_lots = 0);

  [[nodiscard]] const Order *find_order(OrderId order_id) const;
  [[nodiscard]] const Order *find_active_order(OrderId order_id) const;
  [[nodiscard]] bool is_active(OrderId order_id) const;
  [[nodiscard]] std::size_t active_count() const;
  [[nodiscard]] std::vector<OrderId> active_order_ids() const;
  [[nodiscard]] Quantity active_exposure_lots() const;
  [[nodiscard]] const std::vector<OrderLifecycleEvent> &event_log() const;

  void write_order_log_csv(const std::filesystem::path &path) const;

private:
  [[nodiscard]] std::string validate_submit(OrderSide side, Price price_ticks,
                                            Quantity quantity_lots, const book::OrderBook *book,
                                            Quantity current_inventory_lots) const;
  [[nodiscard]] Quantity active_remaining_lots(OrderSide side) const;
  [[nodiscard]] Quantity worst_case_inventory_after_fill(OrderSide side, Quantity quantity_lots,
                                                         Quantity current_inventory_lots) const;
  void append_event(std::int64_t ts_ns, OrderLifecycleEventType event_type, const Order &order,
                    std::optional<OrderId> related_order_id = std::nullopt,
                    std::string reason = {});

  OrderManagerConfig config_;
  OrderId next_order_id_ = 1;
  std::map<OrderId, Order> orders_;
  std::set<OrderId> active_order_ids_;
  std::vector<OrderLifecycleEvent> event_log_;
};

[[nodiscard]] const char *to_string(OrderSide side);
[[nodiscard]] const char *to_string(OrderStatus status);
[[nodiscard]] const char *to_string(OrderLifecycleEventType event_type);

} // namespace lob::execution
