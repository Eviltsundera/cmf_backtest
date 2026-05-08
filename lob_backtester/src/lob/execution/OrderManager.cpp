#include "lob/execution/OrderManager.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lob::execution {
namespace {

Quantity signed_quantity(const OrderSide side, const Quantity quantity_lots) {
  return side == OrderSide::Buy ? quantity_lots : -quantity_lots;
}

Quantity abs_quantity(const Quantity quantity_lots) {
  return quantity_lots < 0 ? -quantity_lots : quantity_lots;
}

std::string csv_escape(std::string value) {
  bool needs_quotes = false;
  for (char &ch : value) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
      needs_quotes = true;
    }
    if (ch == ',' || ch == '"') {
      needs_quotes = true;
    }
  }

  if (!needs_quotes) {
    return value;
  }

  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped += ch;
    }
  }
  escaped += '"';
  return escaped;
}

} // namespace

OrderIntent OrderIntent::submit_limit(const StrategyId strategy_id, const OrderSide side,
                                      const Price price_ticks, const Quantity quantity_lots,
                                      const std::int64_t ts_ns) {
  OrderIntent intent;
  intent.type = OrderIntentType::SubmitLimit;
  intent.strategy_id = strategy_id;
  intent.side = side;
  intent.price_ticks = price_ticks;
  intent.quantity_lots = quantity_lots;
  intent.ts_ns = ts_ns;
  return intent;
}

OrderIntent OrderIntent::cancel(const OrderId order_id, const std::int64_t ts_ns) {
  OrderIntent intent;
  intent.type = OrderIntentType::Cancel;
  intent.order_id = order_id;
  intent.ts_ns = ts_ns;
  return intent;
}

OrderIntent OrderIntent::cancel_all(const StrategyId strategy_id, const std::int64_t ts_ns) {
  OrderIntent intent;
  intent.type = OrderIntentType::CancelAll;
  intent.strategy_id = strategy_id;
  intent.ts_ns = ts_ns;
  return intent;
}

OrderIntent OrderIntent::replace(const OrderId order_id, const Price price_ticks,
                                 const Quantity quantity_lots, const std::int64_t ts_ns) {
  OrderIntent intent;
  intent.type = OrderIntentType::Replace;
  intent.order_id = order_id;
  intent.price_ticks = price_ticks;
  intent.quantity_lots = quantity_lots;
  intent.ts_ns = ts_ns;
  return intent;
}

OrderManager::OrderManager(OrderManagerConfig config) : config_(config) {
  if (config_.risk.max_inventory_lots < 0) {
    throw std::runtime_error("OrderManager max_inventory_lots must be non-negative");
  }
  if (config_.risk.min_quantity_lots <= 0) {
    throw std::runtime_error("OrderManager min_quantity_lots must be positive");
  }
  if (config_.risk.price_tick_multiple <= 0) {
    throw std::runtime_error("OrderManager price_tick_multiple must be positive");
  }
}

const Order &OrderManager::submit_limit(const StrategyId strategy_id, const OrderSide side,
                                        const Price price_ticks, const Quantity quantity_lots,
                                        const std::int64_t ts_ns, const book::OrderBook *book,
                                        const Quantity current_inventory_lots,
                                        const std::optional<OrderId> replaces_order_id) {
  Order order;
  order.id = next_order_id_++;
  order.strategy_id = strategy_id;
  order.side = side;
  order.price_ticks = price_ticks;
  order.quantity_lots = quantity_lots;
  order.remaining_lots = quantity_lots > 0 ? quantity_lots : 0;
  order.ts_submit_ns = ts_ns;
  order.replaces_order_id = replaces_order_id;

  order.reject_reason =
      validate_submit(side, price_ticks, quantity_lots, book, current_inventory_lots);
  if (order.reject_reason.empty()) {
    order.status = OrderStatus::Active;
  } else {
    order.status = OrderStatus::Rejected;
    order.remaining_lots = 0;
  }

  const OrderId order_id = order.id;
  const auto [it, inserted] = orders_.emplace(order_id, std::move(order));
  if (!inserted) {
    throw std::runtime_error("OrderManager generated duplicate order id");
  }

  if (it->second.status == OrderStatus::Active) {
    active_order_ids_.insert(order_id);
    append_event(ts_ns, OrderLifecycleEventType::Submitted, it->second, replaces_order_id);
  } else {
    append_event(ts_ns, OrderLifecycleEventType::Rejected, it->second, replaces_order_id,
                 it->second.reject_reason);
  }

  return it->second;
}

const Order *OrderManager::cancel(const OrderId order_id, const std::int64_t ts_ns) {
  auto it = orders_.find(order_id);
  if (it == orders_.end() || !is_active(order_id)) {
    return nullptr;
  }

  it->second.status = OrderStatus::Cancelled;
  it->second.remaining_lots = 0;
  active_order_ids_.erase(order_id);
  append_event(ts_ns, OrderLifecycleEventType::Cancelled, it->second);
  return &it->second;
}

std::vector<OrderId> OrderManager::cancel_all(const StrategyId strategy_id,
                                              const std::int64_t ts_ns) {
  std::vector<OrderId> cancelled_order_ids;
  std::vector<OrderId> active_snapshot(active_order_ids_.begin(), active_order_ids_.end());

  for (const OrderId order_id : active_snapshot) {
    auto it = orders_.find(order_id);
    if (it != orders_.end() && it->second.strategy_id == strategy_id) {
      if (cancel(order_id, ts_ns) != nullptr) {
        cancelled_order_ids.push_back(order_id);
      }
    }
  }

  return cancelled_order_ids;
}

const Order *OrderManager::replace(const OrderId order_id, const Price price_ticks,
                                   const Quantity quantity_lots, const std::int64_t ts_ns,
                                   const book::OrderBook *book,
                                   const Quantity current_inventory_lots) {
  const Order *existing = find_active_order(order_id);
  if (existing == nullptr) {
    return nullptr;
  }

  const StrategyId strategy_id = existing->strategy_id;
  const OrderSide side = existing->side;
  if (cancel(order_id, ts_ns) == nullptr) {
    return nullptr;
  }

  return &submit_limit(strategy_id, side, price_ticks, quantity_lots, ts_ns, book,
                       current_inventory_lots, order_id);
}

const Order *OrderManager::fill(const OrderId order_id, const std::int64_t ts_ns) {
  auto it = orders_.find(order_id);
  if (it == orders_.end() || !is_active(order_id)) {
    return nullptr;
  }

  it->second.status = OrderStatus::Filled;
  it->second.remaining_lots = 0;
  active_order_ids_.erase(order_id);
  append_event(ts_ns, OrderLifecycleEventType::Filled, it->second);
  return &it->second;
}

OrderIntentResult OrderManager::process_intent(const OrderIntent &intent,
                                               const book::OrderBook *book,
                                               const Quantity current_inventory_lots) {
  OrderIntentResult result;

  switch (intent.type) {
  case OrderIntentType::SubmitLimit: {
    const Order &order =
        submit_limit(intent.strategy_id, intent.side, intent.price_ticks, intent.quantity_lots,
                     intent.ts_ns, book, current_inventory_lots);
    result.order_ids.push_back(order.id);
    result.accepted = order.status == OrderStatus::Active;
    result.reason = order.reject_reason;
    return result;
  }
  case OrderIntentType::Cancel: {
    const Order *order = cancel(intent.order_id, intent.ts_ns);
    result.accepted = order != nullptr;
    if (order != nullptr) {
      result.order_ids.push_back(order->id);
    }
    return result;
  }
  case OrderIntentType::CancelAll:
    result.order_ids = cancel_all(intent.strategy_id, intent.ts_ns);
    result.accepted = !result.order_ids.empty();
    return result;
  case OrderIntentType::Replace: {
    const Order *order = replace(intent.order_id, intent.price_ticks, intent.quantity_lots,
                                 intent.ts_ns, book, current_inventory_lots);
    result.accepted = order != nullptr && order->status == OrderStatus::Active;
    if (order != nullptr) {
      result.order_ids.push_back(order->id);
      result.reason = order->reject_reason;
    }
    return result;
  }
  }

  return result;
}

const Order *OrderManager::find_order(const OrderId order_id) const {
  const auto it = orders_.find(order_id);
  if (it == orders_.end()) {
    return nullptr;
  }
  return &it->second;
}

const Order *OrderManager::find_active_order(const OrderId order_id) const {
  const Order *order = find_order(order_id);
  if (order == nullptr || !is_active(order_id)) {
    return nullptr;
  }
  return order;
}

bool OrderManager::is_active(const OrderId order_id) const {
  return active_order_ids_.contains(order_id);
}

std::size_t OrderManager::active_count() const {
  return active_order_ids_.size();
}

Quantity OrderManager::active_exposure_lots() const {
  Quantity exposure = 0;
  for (const OrderId order_id : active_order_ids_) {
    const auto it = orders_.find(order_id);
    if (it != orders_.end()) {
      exposure += signed_quantity(it->second.side, it->second.remaining_lots);
    }
  }
  return exposure;
}

const std::vector<OrderLifecycleEvent> &OrderManager::event_log() const {
  return event_log_;
}

void OrderManager::write_order_log_csv(const std::filesystem::path &path) const {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Unable to open order log: " + path.string());
  }

  out << "ts_ns,event_type,order_id,strategy_id,side,price_ticks,quantity_lots,"
         "remaining_lots,status,related_order_id,reason\n";
  for (const OrderLifecycleEvent &event : event_log_) {
    out << event.ts_ns << ',' << to_string(event.event_type) << ',' << event.order_id << ','
        << event.strategy_id << ',' << to_string(event.side) << ',' << event.price_ticks << ','
        << event.quantity_lots << ',' << event.remaining_lots << ',' << to_string(event.status)
        << ',';
    if (event.related_order_id) {
      out << *event.related_order_id;
    }
    out << ',' << csv_escape(event.reason) << '\n';
  }
}

std::string OrderManager::validate_submit(const OrderSide side, const Price price_ticks,
                                          const Quantity quantity_lots, const book::OrderBook *book,
                                          const Quantity current_inventory_lots) const {
  if (price_ticks <= 0) {
    return "price must be positive";
  }
  if (quantity_lots < config_.risk.min_quantity_lots) {
    return "quantity below min_qty";
  }
  if (price_ticks % config_.risk.price_tick_multiple != 0) {
    return "price is not tick aligned";
  }
  if (abs_quantity(projected_inventory_after_fill(side, quantity_lots, current_inventory_lots)) >
      config_.risk.max_inventory_lots) {
    return "max_inventory exceeded";
  }

  if (config_.risk.strict_maker && book != nullptr) {
    if (side == OrderSide::Buy) {
      const auto best_ask = book->best_ask();
      if (best_ask && price_ticks >= best_ask->price_ticks) {
        return "strict_maker buy crosses best ask";
      }
    } else {
      const auto best_bid = book->best_bid();
      if (best_bid && price_ticks <= best_bid->price_ticks) {
        return "strict_maker sell crosses best bid";
      }
    }
  }

  return {};
}

Quantity OrderManager::projected_inventory_after_fill(const OrderSide side,
                                                      const Quantity quantity_lots,
                                                      const Quantity current_inventory_lots) const {
  return current_inventory_lots + active_exposure_lots() + signed_quantity(side, quantity_lots);
}

void OrderManager::append_event(const std::int64_t ts_ns, const OrderLifecycleEventType event_type,
                                const Order &order, const std::optional<OrderId> related_order_id,
                                std::string reason) {
  OrderLifecycleEvent event;
  event.ts_ns = ts_ns;
  event.event_type = event_type;
  event.order_id = order.id;
  event.strategy_id = order.strategy_id;
  event.side = order.side;
  event.price_ticks = order.price_ticks;
  event.quantity_lots = order.quantity_lots;
  event.remaining_lots = order.remaining_lots;
  event.status = order.status;
  event.related_order_id = related_order_id;
  event.reason = std::move(reason);
  event_log_.push_back(std::move(event));
}

const char *to_string(const OrderSide side) {
  switch (side) {
  case OrderSide::Buy:
    return "buy";
  case OrderSide::Sell:
    return "sell";
  }
  return "unknown";
}

const char *to_string(const OrderStatus status) {
  switch (status) {
  case OrderStatus::Active:
    return "active";
  case OrderStatus::Filled:
    return "filled";
  case OrderStatus::Cancelled:
    return "cancelled";
  case OrderStatus::Rejected:
    return "rejected";
  }
  return "unknown";
}

const char *to_string(const OrderLifecycleEventType event_type) {
  switch (event_type) {
  case OrderLifecycleEventType::Submitted:
    return "submitted";
  case OrderLifecycleEventType::Rejected:
    return "rejected";
  case OrderLifecycleEventType::Cancelled:
    return "cancelled";
  case OrderLifecycleEventType::Filled:
    return "filled";
  }
  return "unknown";
}

} // namespace lob::execution
