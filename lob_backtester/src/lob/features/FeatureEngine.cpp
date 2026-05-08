#include "lob/features/FeatureEngine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lob::features {
namespace {

std::optional<std::pair<book::BookLevel, book::BookLevel>>
best_bid_ask(const book::OrderBook &book) {
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return std::pair<book::BookLevel, book::BookLevel>{*bid, *ask};
}

} // namespace

std::optional<double> mid(const book::OrderBook &book) {
  const auto levels = best_bid_ask(book);
  if (!levels) {
    return std::nullopt;
  }

  return (static_cast<double>(levels->first.price_ticks) +
          static_cast<double>(levels->second.price_ticks)) /
         2.0;
}

std::optional<double> spread(const book::OrderBook &book) {
  const auto levels = best_bid_ask(book);
  if (!levels) {
    return std::nullopt;
  }

  return static_cast<double>(levels->second.price_ticks - levels->first.price_ticks);
}

std::optional<double> imbalance(const book::OrderBook &book) {
  const auto levels = best_bid_ask(book);
  if (!levels) {
    return std::nullopt;
  }

  const auto bid_quantity = static_cast<double>(levels->first.quantity_lots);
  const auto ask_quantity = static_cast<double>(levels->second.quantity_lots);
  const double denominator = bid_quantity + ask_quantity;
  if (denominator == 0.0) {
    return std::nullopt;
  }

  return (bid_quantity - ask_quantity) / denominator;
}

std::optional<double> weighted_mid(const book::OrderBook &book) {
  const auto levels = best_bid_ask(book);
  if (!levels) {
    return std::nullopt;
  }

  const auto bid_price = static_cast<double>(levels->first.price_ticks);
  const auto ask_price = static_cast<double>(levels->second.price_ticks);
  const auto bid_quantity = static_cast<double>(levels->first.quantity_lots);
  const auto ask_quantity = static_cast<double>(levels->second.quantity_lots);
  const double denominator = bid_quantity + ask_quantity;
  if (denominator == 0.0) {
    return std::nullopt;
  }

  return (ask_price * bid_quantity + bid_price * ask_quantity) / denominator;
}

std::optional<double> microprice_proxy(const book::OrderBook &book, const double alpha) {
  const auto current_mid = mid(book);
  const auto current_spread = spread(book);
  const auto current_imbalance = imbalance(book);
  if (!current_mid || !current_spread || !current_imbalance) {
    return std::nullopt;
  }

  return *current_mid + alpha * (*current_spread / 2.0) * *current_imbalance;
}

RollingStd::RollingStd(const std::size_t window_size) : window_size_(window_size) {
  if (window_size_ == 0) {
    throw std::runtime_error("RollingStd window_size must be positive");
  }
}

double RollingStd::push(const double value) {
  values_.push_back(value);
  sum_ += value;
  sum_squares_ += static_cast<long double>(value) * static_cast<long double>(value);
  pop_oldest_if_needed();
  return *this->value();
}

void RollingStd::clear() {
  values_.clear();
  sum_ = 0.0L;
  sum_squares_ = 0.0L;
}

std::optional<double> RollingStd::value() const {
  if (values_.empty()) {
    return std::nullopt;
  }

  const auto count_value = static_cast<long double>(values_.size());
  const long double mean = sum_ / count_value;
  const long double variance = std::max(0.0L, (sum_squares_ / count_value) - (mean * mean));
  return std::sqrt(static_cast<double>(variance));
}

std::size_t RollingStd::count() const {
  return values_.size();
}

std::size_t RollingStd::window_size() const {
  return window_size_;
}

void RollingStd::pop_oldest_if_needed() {
  while (values_.size() > window_size_) {
    const double oldest = values_.front();
    values_.pop_front();
    sum_ -= oldest;
    sum_squares_ -= static_cast<long double>(oldest) * static_cast<long double>(oldest);
  }
}

RollingMidReturnStd::RollingMidReturnStd(const std::size_t window_size) : returns_(window_size) {
}

std::optional<double> RollingMidReturnStd::push_mid(const double mid_price) {
  if (mid_price <= 0.0) {
    throw std::runtime_error("RollingMidReturnStd mid_price must be positive");
  }

  if (!previous_mid_) {
    previous_mid_ = mid_price;
    return std::nullopt;
  }

  const double mid_return = (mid_price - *previous_mid_) / *previous_mid_;
  previous_mid_ = mid_price;
  return returns_.push(mid_return);
}

void RollingMidReturnStd::clear() {
  previous_mid_.reset();
  returns_.clear();
}

std::optional<double> RollingMidReturnStd::value() const {
  return returns_.value();
}

std::size_t RollingMidReturnStd::count() const {
  return returns_.count();
}

std::size_t RollingMidReturnStd::window_size() const {
  return returns_.window_size();
}

} // namespace lob::features
