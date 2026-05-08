#include "lob/portfolio/Portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lob::portfolio {
namespace {

using execution::Quantity;

Quantity abs_quantity(const Quantity quantity_lots) {
  return quantity_lots < 0 ? -quantity_lots : quantity_lots;
}

int quantity_sign(const Quantity quantity_lots) {
  if (quantity_lots > 0) {
    return 1;
  }
  if (quantity_lots < 0) {
    return -1;
  }
  return 0;
}

void validate_fill(const execution::Fill &fill) {
  if (fill.fill_price_ticks <= 0) {
    throw std::runtime_error("Portfolio fill price must be positive");
  }
  if (fill.quantity_lots <= 0) {
    throw std::runtime_error("Portfolio fill quantity must be positive");
  }
  if (!std::isfinite(fill.fee)) {
    throw std::runtime_error("Portfolio fill fee must be finite");
  }
}

} // namespace

Portfolio::Portfolio(const double initial_cash) : initial_cash_(initial_cash), cash_(initial_cash) {
  if (!std::isfinite(initial_cash_)) {
    throw std::runtime_error("Portfolio initial cash must be finite");
  }
}

void Portfolio::apply_fill(const execution::Fill &fill) {
  validate_fill(fill);

  const Quantity fill_quantity = signed_fill_quantity(fill);
  const double fill_price = static_cast<double>(fill.fill_price_ticks);
  cash_ -= static_cast<double>(fill_quantity) * fill_price;
  cash_ -= fill.fee;

  realized_pnl_ -= fill.fee;

  if (position_lots_ == 0 || quantity_sign(position_lots_) == quantity_sign(fill_quantity)) {
    const Quantity new_position = position_lots_ + fill_quantity;
    const double weighted_notional =
        average_entry_price_ * static_cast<double>(abs_quantity(position_lots_)) +
        fill_price * static_cast<double>(abs_quantity(fill_quantity));
    average_entry_price_ = weighted_notional / static_cast<double>(abs_quantity(new_position));
    position_lots_ = new_position;
    return;
  }

  const Quantity closed_lots = std::min(abs_quantity(position_lots_), abs_quantity(fill_quantity));
  realized_pnl_ += static_cast<double>(closed_lots) * (fill_price - average_entry_price_) *
                   static_cast<double>(quantity_sign(position_lots_));

  const Quantity new_position = position_lots_ + fill_quantity;
  if (new_position == 0) {
    position_lots_ = 0;
    average_entry_price_ = 0.0;
    return;
  }

  if (quantity_sign(new_position) == quantity_sign(position_lots_)) {
    position_lots_ = new_position;
    return;
  }

  position_lots_ = new_position;
  average_entry_price_ = fill_price;
}

double Portfolio::cash() const {
  return cash_;
}

execution::Quantity Portfolio::position_lots() const {
  return position_lots_;
}

double Portfolio::average_entry_price() const {
  return average_entry_price_;
}

double Portfolio::realized_pnl() const {
  return realized_pnl_;
}

double Portfolio::unrealized_pnl(const double mark_price) const {
  validate_mark_price(mark_price);
  if (position_lots_ == 0) {
    return 0.0;
  }
  return static_cast<double>(position_lots_) * (mark_price - average_entry_price_);
}

double Portfolio::equity(const double mark_price) const {
  validate_mark_price(mark_price);
  return cash_ + static_cast<double>(position_lots_) * mark_price;
}

double Portfolio::total_pnl(const double mark_price) const {
  return equity(mark_price) - initial_cash_;
}

double Portfolio::initial_cash() const {
  return initial_cash_;
}

void Portfolio::validate_mark_price(const double mark_price) const {
  if (!std::isfinite(mark_price) || mark_price <= 0.0) {
    throw std::runtime_error("Portfolio mark price must be finite and positive");
  }
}

execution::Quantity signed_fill_quantity(const execution::Fill &fill) {
  return fill.side == execution::OrderSide::Buy ? fill.quantity_lots : -fill.quantity_lots;
}

} // namespace lob::portfolio
