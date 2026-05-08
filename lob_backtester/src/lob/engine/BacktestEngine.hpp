#pragma once

#include "lob/book/OrderBook.hpp"
#include "lob/data/DataSource.hpp"
#include "lob/execution/FillModel.hpp"
#include "lob/metrics/MetricsEngine.hpp"
#include "lob/portfolio/Portfolio.hpp"
#include "lob/strategies/Strategy.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace lob::engine {

struct BacktestEngineConfig {
  book::OrderBookConfig book;
  execution::OrderManagerConfig orders;
  execution::FillModelConfig fills;
  double initial_cash = 0.0;
  std::int64_t quote_refresh_ns = 0;
  std::vector<std::int64_t> adverse_selection_horizons_ns = {1'000'000'000, 10'000'000'000};
  std::filesystem::path output_dir;
};

struct BacktestResult {
  data::EventCounts event_counts;
  std::size_t fill_count = 0;
  std::size_t order_event_count = 0;
  std::size_t active_order_count = 0;
  double elapsed_seconds = 0.0;
  double events_per_second = 0.0;
  portfolio::Portfolio portfolio;
  metrics::MetricsSnapshot metrics;
  std::vector<execution::Fill> fills;
};

class BacktestEngine {
public:
  explicit BacktestEngine(BacktestEngineConfig config = {});

  BacktestResult run(data::IDataSource &source, strategies::IStrategy &strategy) const;

private:
  BacktestEngineConfig config_;
};

[[nodiscard]] strategies::MarketState make_market_state(const data::MarketEvent &event,
                                                        const book::OrderBook &book,
                                                        const execution::OrderManager &orders,
                                                        const portfolio::Portfolio &portfolio);

void write_fills_csv(const std::filesystem::path &path, const std::vector<execution::Fill> &fills);

} // namespace lob::engine
