#include "lob/execution/FillModel.hpp"

#include <cmath>
#include <stdexcept>

namespace lob::execution {
namespace {

bool should_fill(const OrderSide side, const double reference_price, const Price limit_price) {
  if (side == OrderSide::Buy) {
    return reference_price <= static_cast<double>(limit_price);
  }
  return reference_price >= static_cast<double>(limit_price);
}

double bps_to_rate(const double bps) {
  return bps / 10000.0;
}

} // namespace

FillModel::FillModel(FillModelConfig config) : config_(config) {
  if (!std::isfinite(config_.maker_bps) || !std::isfinite(config_.taker_bps)) {
    throw std::runtime_error("FillModel fees must be finite");
  }
  if (config_.latency_ns < 0) {
    throw std::runtime_error("FillModel latency_ns must be non-negative");
  }
}

std::optional<Fill> FillModel::check_fill(const Order &order, const data::MarketEvent &event,
                                          const book::OrderBook &book) const {
  if (order.status != OrderStatus::Active || order.remaining_lots <= 0) {
    return std::nullopt;
  }

  const auto reference = reference_price(order.side, event, book);
  if (!reference || !should_fill(order.side, *reference, order.price_ticks)) {
    return std::nullopt;
  }

  Fill fill;
  fill.order_id = order.id;
  fill.strategy_id = order.strategy_id;
  fill.side = order.side;
  fill.limit_price_ticks = order.price_ticks;
  fill.fill_price_ticks = order.price_ticks;
  fill.quantity_lots = order.remaining_lots;
  fill.ts_ns = event.ts_ns;
  fill.liquidity_role = config_.default_role;
  fill.fee = fee_for_fill(fill.fill_price_ticks, fill.quantity_lots);
  return fill;
}

std::vector<Fill> FillModel::fill_active_orders(OrderManager &orders,
                                                const data::MarketEvent &event,
                                                const book::OrderBook &book) const {
  std::vector<Fill> fills;
  const std::vector<OrderId> active_order_ids = orders.active_order_ids();
  fills.reserve(active_order_ids.size());

  for (const OrderId order_id : active_order_ids) {
    const Order *order = orders.find_active_order(order_id);
    if (order == nullptr) {
      continue;
    }

    const std::optional<Fill> fill = check_fill(*order, event, book);
    if (!fill) {
      continue;
    }

    if (orders.fill(order_id, event.ts_ns) != nullptr) {
      fills.push_back(*fill);
    }
  }

  return fills;
}

std::optional<double> FillModel::reference_price(const OrderSide side,
                                                 const data::MarketEvent &event,
                                                 const book::OrderBook &book) const {
  switch (config_.fill_reference) {
  case FillReference::TradePrice:
    return trade_or_best_quote_reference(side, event, book);
  case FillReference::BestQuote:
    return best_quote_reference(side, book);
  case FillReference::MidPrice:
    return book.mid();
  }
  return std::nullopt;
}

std::optional<double> FillModel::trade_or_best_quote_reference(const OrderSide side,
                                                               const data::MarketEvent &event,
                                                               const book::OrderBook &book) const {
  if (event.type == data::EventType::Trade) {
    return static_cast<double>(event.payload.trade.price_ticks);
  }
  return best_quote_reference(side, book);
}

std::optional<double> FillModel::best_quote_reference(const OrderSide side,
                                                      const book::OrderBook &book) const {
  if (side == OrderSide::Buy) {
    const auto ask = book.best_ask();
    if (!ask) {
      return std::nullopt;
    }
    return static_cast<double>(ask->price_ticks);
  }

  const auto bid = book.best_bid();
  if (!bid) {
    return std::nullopt;
  }
  return static_cast<double>(bid->price_ticks);
}

double FillModel::fee_for_fill(const Price fill_price_ticks, const Quantity quantity_lots) const {
  const double notional =
      std::abs(static_cast<double>(fill_price_ticks) * static_cast<double>(quantity_lots));
  const double fee_bps =
      config_.default_role == LiquidityRole::Maker ? config_.maker_bps : config_.taker_bps;
  return notional * bps_to_rate(fee_bps);
}

const char *to_string(const FillReference reference) {
  switch (reference) {
  case FillReference::TradePrice:
    return "trade_price";
  case FillReference::BestQuote:
    return "best_quote";
  case FillReference::MidPrice:
    return "mid_price";
  }
  return "unknown";
}

const char *to_string(const LiquidityRole role) {
  switch (role) {
  case LiquidityRole::Maker:
    return "maker";
  case LiquidityRole::Taker:
    return "taker";
  }
  return "unknown";
}

} // namespace lob::execution
