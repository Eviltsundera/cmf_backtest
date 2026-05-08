#pragma once

#include "lob/execution/FillModel.hpp"

#include <cstdint>

namespace lob::portfolio {

class Portfolio {
public:
  explicit Portfolio(double initial_cash = 0.0);

  void apply_fill(const execution::Fill &fill);

  [[nodiscard]] double cash() const;
  [[nodiscard]] execution::Quantity position_lots() const;
  [[nodiscard]] double average_entry_price() const;
  [[nodiscard]] double realized_pnl() const;
  [[nodiscard]] double unrealized_pnl(double mark_price) const;
  [[nodiscard]] double equity(double mark_price) const;
  [[nodiscard]] double total_pnl(double mark_price) const;
  [[nodiscard]] double initial_cash() const;

private:
  void validate_mark_price(double mark_price) const;

  double initial_cash_ = 0.0;
  double cash_ = 0.0;
  execution::Quantity position_lots_ = 0;
  double average_entry_price_ = 0.0;
  double realized_pnl_ = 0.0;
};

[[nodiscard]] execution::Quantity signed_fill_quantity(const execution::Fill &fill);

} // namespace lob::portfolio
