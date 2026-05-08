#include "lob/metrics/MetricsEngine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>

namespace lob::metrics {
namespace {

void validate_price(const double price, const char *name) {
  if (!std::isfinite(price) || price <= 0.0) {
    throw std::runtime_error(std::string(name) + " must be finite and positive");
  }
}

void ensure_parent_directory(const std::filesystem::path &path) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
}

void ensure_output_stream(const std::ofstream &out, const std::filesystem::path &path) {
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path.string());
  }
}

execution::Quantity abs_quantity(const execution::Quantity quantity_lots) {
  return quantity_lots < 0 ? -quantity_lots : quantity_lots;
}

int fill_direction(const execution::Fill &fill) {
  return fill.side == execution::OrderSide::Buy ? 1 : -1;
}

void write_json_field(std::ostream &out, const char *name, const double value,
                      const bool trailing_comma = true) {
  out << "  \"" << name << "\": " << value;
  if (trailing_comma) {
    out << ',';
  }
  out << '\n';
}

void write_json_field(std::ostream &out, const char *name, const std::size_t value,
                      const bool trailing_comma = true) {
  out << "  \"" << name << "\": " << value;
  if (trailing_comma) {
    out << ',';
  }
  out << '\n';
}

void write_json_field(std::ostream &out, const char *name, const execution::Quantity value,
                      const bool trailing_comma = true) {
  out << "  \"" << name << "\": " << value;
  if (trailing_comma) {
    out << ',';
  }
  out << '\n';
}

} // namespace

void MetricsEngine::RunningAverage::push(const double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("MetricsEngine average input must be finite");
  }
  sum += value;
  ++count;
}

double MetricsEngine::RunningAverage::mean() const {
  if (count == 0) {
    return 0.0;
  }
  return sum / static_cast<double>(count);
}

void MetricsEngine::record_equity(const std::int64_t ts_ns, const portfolio::Portfolio &portfolio,
                                  const double mark_price) {
  EquityPoint point;
  point.ts_ns = ts_ns;
  point.mark_price = mark_price;
  point.equity = portfolio.equity(mark_price);
  point.total_pnl = portfolio.total_pnl(mark_price);
  point.realized_pnl = portfolio.realized_pnl();
  point.unrealized_pnl = portfolio.unrealized_pnl(mark_price);
  point.cash = portfolio.cash();
  point.position_lots = portfolio.position_lots();
  equity_curve_.push_back(point);
}

void MetricsEngine::record_fill(const execution::Fill &fill,
                                const std::optional<double> reference_mid_price) {
  if (fill.fill_price_ticks <= 0) {
    throw std::runtime_error("MetricsEngine fill price must be positive");
  }
  if (fill.quantity_lots <= 0) {
    throw std::runtime_error("MetricsEngine fill quantity must be positive");
  }
  if (!std::isfinite(fill.fee)) {
    throw std::runtime_error("MetricsEngine fill fee must be finite");
  }

  turnover_qty_ += fill.quantity_lots;
  turnover_notional_ += std::abs(static_cast<double>(fill.fill_price_ticks) *
                                 static_cast<double>(fill.quantity_lots));
  ++fill_count_;

  if (reference_mid_price) {
    spread_captured_.push(spread_captured(fill, *reference_mid_price));
  }
}

void MetricsEngine::record_fill_opportunity() {
  ++fill_opportunity_count_;
}

void MetricsEngine::set_fill_opportunity_count(const std::size_t fill_opportunity_count) {
  fill_opportunity_count_ = fill_opportunity_count;
}

void MetricsEngine::record_quote(const std::int64_t, const std::optional<double> bid_price,
                                 const std::optional<double> ask_price) {
  if (bid_price) {
    validate_price(*bid_price, "bid price");
  }
  if (ask_price) {
    validate_price(*ask_price, "ask price");
  }
  if (bid_price && ask_price && *ask_price <= *bid_price) {
    throw std::runtime_error("MetricsEngine quote ask must be greater than bid");
  }

  ++quote_sample_count_;
  if (!bid_price || !ask_price) {
    return;
  }

  ++active_quote_sample_count_;
  quoted_spread_.push(*ask_price - *bid_price);
}

void MetricsEngine::record_adverse_selection(const execution::Fill &fill,
                                             const std::int64_t horizon_ns,
                                             const double future_mid_price) {
  if (horizon_ns <= 0) {
    throw std::runtime_error("MetricsEngine adverse-selection horizon must be positive");
  }
  const double markout = adverse_selection_markout(fill, future_mid_price);
  adverse_selection_by_horizon_[horizon_ns].push(markout);
}

MetricsSnapshot MetricsEngine::compute() const {
  MetricsSnapshot snapshot;
  if (!equity_curve_.empty()) {
    snapshot.final_pnl = equity_curve_.back().total_pnl;
  }

  snapshot.mean_inventory = compute_mean_inventory();
  snapshot.max_inventory = compute_max_inventory();
  snapshot.inventory_std = compute_inventory_std(snapshot.mean_inventory);
  snapshot.turnover_qty = turnover_qty_;
  snapshot.turnover_notional = turnover_notional_;
  snapshot.fill_count = fill_count_;
  if (fill_opportunity_count_ > 0) {
    snapshot.fill_rate =
        static_cast<double>(fill_count_) / static_cast<double>(fill_opportunity_count_);
  }
  snapshot.max_drawdown = compute_max_drawdown();
  snapshot.avg_quoted_spread = quoted_spread_.mean();
  snapshot.avg_spread_captured = spread_captured_.mean();
  if (quote_sample_count_ > 0) {
    snapshot.quote_uptime =
        static_cast<double>(active_quote_sample_count_) / static_cast<double>(quote_sample_count_);
  }

  for (const auto &[horizon_ns, running_average] : adverse_selection_by_horizon_) {
    snapshot.adverse_selection_h[horizon_ns] = running_average.mean();
  }
  return snapshot;
}

const std::vector<EquityPoint> &MetricsEngine::equity_curve() const {
  return equity_curve_;
}

void MetricsEngine::write_metrics_json(const std::filesystem::path &path) const {
  ensure_parent_directory(path);
  std::ofstream out(path);
  ensure_output_stream(out, path);
  out << std::setprecision(12);

  const MetricsSnapshot snapshot = compute();
  out << "{\n";
  write_json_field(out, "final_pnl", snapshot.final_pnl);
  write_json_field(out, "mean_inventory", snapshot.mean_inventory);
  write_json_field(out, "max_inventory", snapshot.max_inventory);
  write_json_field(out, "inventory_std", snapshot.inventory_std);
  write_json_field(out, "turnover_qty", snapshot.turnover_qty);
  write_json_field(out, "turnover_notional", snapshot.turnover_notional);
  write_json_field(out, "fill_count", snapshot.fill_count);
  write_json_field(out, "fill_rate", snapshot.fill_rate);
  write_json_field(out, "max_drawdown", snapshot.max_drawdown);
  write_json_field(out, "avg_quoted_spread", snapshot.avg_quoted_spread);
  write_json_field(out, "avg_spread_captured", snapshot.avg_spread_captured);
  write_json_field(out, "quote_uptime", snapshot.quote_uptime);
  out << "  \"adverse_selection_h\": {";
  if (!snapshot.adverse_selection_h.empty()) {
    out << '\n';
    auto it = snapshot.adverse_selection_h.begin();
    while (it != snapshot.adverse_selection_h.end()) {
      out << "    \"" << it->first << "\": " << it->second;
      ++it;
      if (it != snapshot.adverse_selection_h.end()) {
        out << ',';
      }
      out << '\n';
    }
    out << "  }\n";
  } else {
    out << "}\n";
  }
  out << "}\n";
}

void MetricsEngine::write_equity_curve_csv(const std::filesystem::path &path) const {
  ensure_parent_directory(path);
  std::ofstream out(path);
  ensure_output_stream(out, path);
  out << std::setprecision(12);
  out << "ts_ns,mark_price,equity,total_pnl,realized_pnl,unrealized_pnl,cash,position_lots\n";
  for (const EquityPoint &point : equity_curve_) {
    out << point.ts_ns << ',' << point.mark_price << ',' << point.equity << ',' << point.total_pnl
        << ',' << point.realized_pnl << ',' << point.unrealized_pnl << ',' << point.cash << ','
        << point.position_lots << '\n';
  }
}

void MetricsEngine::write_inventory_csv(const std::filesystem::path &path) const {
  ensure_parent_directory(path);
  std::ofstream out(path);
  ensure_output_stream(out, path);
  out << "ts_ns,position_lots\n";
  for (const EquityPoint &point : equity_curve_) {
    out << point.ts_ns << ',' << point.position_lots << '\n';
  }
}

void MetricsEngine::write_run_outputs(const std::filesystem::path &directory) const {
  std::filesystem::create_directories(directory);
  write_metrics_json(directory / "metrics.json");
  write_equity_curve_csv(directory / "equity_curve.csv");
  write_inventory_csv(directory / "inventory.csv");
}

double MetricsEngine::compute_mean_inventory() const {
  if (equity_curve_.empty()) {
    return 0.0;
  }

  double sum = 0.0;
  for (const EquityPoint &point : equity_curve_) {
    sum += static_cast<double>(point.position_lots);
  }
  return sum / static_cast<double>(equity_curve_.size());
}

execution::Quantity MetricsEngine::compute_max_inventory() const {
  execution::Quantity max_inventory = 0;
  for (const EquityPoint &point : equity_curve_) {
    max_inventory = std::max(max_inventory, abs_quantity(point.position_lots));
  }
  return max_inventory;
}

double MetricsEngine::compute_inventory_std(const double mean_inventory) const {
  if (equity_curve_.empty()) {
    return 0.0;
  }

  double squared_error_sum = 0.0;
  for (const EquityPoint &point : equity_curve_) {
    const double delta = static_cast<double>(point.position_lots) - mean_inventory;
    squared_error_sum += delta * delta;
  }
  return std::sqrt(squared_error_sum / static_cast<double>(equity_curve_.size()));
}

double MetricsEngine::compute_max_drawdown() const {
  if (equity_curve_.empty()) {
    return 0.0;
  }

  double peak = equity_curve_.front().equity;
  double max_drawdown = 0.0;
  for (const EquityPoint &point : equity_curve_) {
    peak = std::max(peak, point.equity);
    max_drawdown = std::max(max_drawdown, peak - point.equity);
  }
  return max_drawdown;
}

double spread_captured(const execution::Fill &fill, const double reference_mid_price) {
  validate_price(reference_mid_price, "reference mid price");
  return -static_cast<double>(fill_direction(fill)) *
         (static_cast<double>(fill.fill_price_ticks) - reference_mid_price);
}

double adverse_selection_markout(const execution::Fill &fill, const double future_mid_price) {
  validate_price(future_mid_price, "future mid price");
  if (fill.fill_price_ticks <= 0) {
    throw std::runtime_error("MetricsEngine fill price must be positive");
  }
  return static_cast<double>(fill_direction(fill)) *
         (future_mid_price - static_cast<double>(fill.fill_price_ticks));
}

} // namespace lob::metrics
