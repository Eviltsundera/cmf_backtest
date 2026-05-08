#pragma once

#include "lob/book/OrderBook.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace lob::features {

[[nodiscard]] std::optional<double> mid(const book::OrderBook &book);
[[nodiscard]] std::optional<double> spread(const book::OrderBook &book);
[[nodiscard]] std::optional<double> imbalance(const book::OrderBook &book);
[[nodiscard]] std::optional<double> weighted_mid(const book::OrderBook &book);
[[nodiscard]] std::optional<double> microprice_proxy(const book::OrderBook &book, double alpha);

class RollingStd {
public:
  explicit RollingStd(std::size_t window_size);

  double push(double value);
  void clear();

  [[nodiscard]] std::optional<double> value() const;
  [[nodiscard]] std::size_t count() const;
  [[nodiscard]] std::size_t window_size() const;

private:
  void pop_oldest_if_needed();

  std::size_t window_size_;
  std::deque<double> values_;
  long double sum_ = 0.0L;
  long double sum_squares_ = 0.0L;
};

class RollingMidReturnStd {
public:
  explicit RollingMidReturnStd(std::size_t window_size);

  std::optional<double> push_mid(double mid_price);
  void clear();

  [[nodiscard]] std::optional<double> value() const;
  [[nodiscard]] std::size_t count() const;
  [[nodiscard]] std::size_t window_size() const;

private:
  std::optional<double> previous_mid_;
  RollingStd returns_;
};

} // namespace lob::features
