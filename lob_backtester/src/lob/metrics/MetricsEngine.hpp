#pragma once

#include "lob/execution/FillModel.hpp"
#include "lob/portfolio/Portfolio.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

namespace lob::metrics {

struct EquityPoint {
  std::int64_t ts_ns = 0;
  double mark_price = 0.0;
  double equity = 0.0;
  double total_pnl = 0.0;
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  double cash = 0.0;
  execution::Quantity position_lots = 0;
};

struct MetricsSnapshot {
  double final_pnl = 0.0;
  double mean_inventory = 0.0;
  execution::Quantity max_inventory = 0;
  double inventory_std = 0.0;
  execution::Quantity turnover_qty = 0;
  double turnover_notional = 0.0;
  std::size_t fill_count = 0;
  double fill_rate = 0.0;
  double max_drawdown = 0.0;
  double avg_quoted_spread = 0.0;
  double avg_spread_captured = 0.0;
  std::map<std::int64_t, double> adverse_selection_h;
  double quote_uptime = 0.0;
};

class MetricsEngine {
public:
  void record_equity(std::int64_t ts_ns, const portfolio::Portfolio &portfolio, double mark_price);
  void record_fill(const execution::Fill &fill,
                   std::optional<double> reference_mid_price = std::nullopt);
  void record_fill_opportunity();
  void set_fill_opportunity_count(std::size_t fill_opportunity_count);
  void record_quote(std::int64_t ts_ns, std::optional<double> bid_price,
                    std::optional<double> ask_price);
  void record_adverse_selection(const execution::Fill &fill, std::int64_t horizon_ns,
                                double future_mid_price);

  [[nodiscard]] MetricsSnapshot compute() const;
  [[nodiscard]] const std::vector<EquityPoint> &equity_curve() const;

  void write_metrics_json(const std::filesystem::path &path) const;
  void write_equity_curve_csv(const std::filesystem::path &path) const;
  void write_inventory_csv(const std::filesystem::path &path) const;
  void write_run_outputs(const std::filesystem::path &directory) const;

private:
  struct RunningAverage {
    double sum = 0.0;
    std::size_t count = 0;

    void push(double value);
    [[nodiscard]] double mean() const;
  };

  [[nodiscard]] double compute_mean_inventory() const;
  [[nodiscard]] execution::Quantity compute_max_inventory() const;
  [[nodiscard]] double compute_inventory_std(double mean_inventory) const;
  [[nodiscard]] double compute_max_drawdown() const;

  std::vector<EquityPoint> equity_curve_;
  execution::Quantity turnover_qty_ = 0;
  double turnover_notional_ = 0.0;
  std::size_t fill_count_ = 0;
  std::size_t fill_opportunity_count_ = 0;
  std::size_t quote_sample_count_ = 0;
  std::size_t active_quote_sample_count_ = 0;
  RunningAverage quoted_spread_;
  RunningAverage spread_captured_;
  std::map<std::int64_t, RunningAverage> adverse_selection_by_horizon_;
};

[[nodiscard]] double spread_captured(const execution::Fill &fill, double reference_mid_price);
[[nodiscard]] double adverse_selection_markout(const execution::Fill &fill,
                                               double future_mid_price);

} // namespace lob::metrics
