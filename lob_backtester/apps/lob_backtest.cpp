#include "lob/data/CsvDataSource.hpp"
#include "lob/engine/BacktestEngine.hpp"
#include "lob/strategies/Strategy.hpp"
#include "lob/utils/Config.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace {

void print_usage(std::ostream &out) {
  out << "Usage: lob_backtest --config <path-to-yaml>\n";
}

std::filesystem::path parse_config_path(const int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      print_usage(std::cout);
      return {};
    }
    if (arg == "--config" && index + 1 < argc) {
      return std::filesystem::path(argv[index + 1]);
    }
  }

  throw std::runtime_error("Missing required --config <path-to-yaml> argument");
}

lob::execution::FillReference parse_fill_reference(const std::string_view value) {
  if (value == "trade_price") {
    return lob::execution::FillReference::TradePrice;
  }
  if (value == "best_quote") {
    return lob::execution::FillReference::BestQuote;
  }
  if (value == "mid_price") {
    return lob::execution::FillReference::MidPrice;
  }
  throw std::runtime_error("Unsupported fill_reference: " + std::string(value));
}

bool is_fixed_spread_strategy(const std::string_view name) {
  return name == "fixed_spread" || name == "baseline_fixed" || name == "fixed";
}

bool is_avellaneda_stoikov_strategy(const std::string_view name) {
  return name == "avellaneda_stoikov" || name == "avellaneda" || name == "as";
}

std::int64_t milliseconds_to_nanoseconds(const std::int64_t milliseconds) {
  constexpr std::int64_t kNsPerMs = 1'000'000;
  if (milliseconds > std::numeric_limits<std::int64_t>::max() / kNsPerMs ||
      milliseconds < std::numeric_limits<std::int64_t>::min() / kNsPerMs) {
    throw std::runtime_error("quote_refresh_ms overflows nanoseconds");
  }
  return milliseconds * kNsPerMs;
}

lob::engine::BacktestEngineConfig
engine_config_from_app_config(const lob::utils::AppConfig &config) {
  if (config.execution.fill_model != "price_cross") {
    throw std::runtime_error("Unsupported fill_model: " + config.execution.fill_model);
  }

  lob::engine::BacktestEngineConfig engine_config;
  engine_config.book.max_depth = config.book.max_depth;
  engine_config.fills.fill_reference = parse_fill_reference(config.execution.fill_reference);
  engine_config.fills.partial_fills = config.execution.partial_fills;
  engine_config.fills.maker_bps = config.execution.maker_bps;
  engine_config.fills.taker_bps = config.execution.taker_bps;
  engine_config.quote_refresh_ns = milliseconds_to_nanoseconds(config.strategy.quote_refresh_ms);
  engine_config.output_dir = config.run.output_dir;
  if (config.strategy.max_inventory > 0) {
    engine_config.orders.risk.max_inventory_lots = config.strategy.max_inventory;
  }
  if (is_fixed_spread_strategy(config.strategy.name) ||
      is_avellaneda_stoikov_strategy(config.strategy.name)) {
    engine_config.orders.risk.strict_maker = true;
  }
  return engine_config;
}

std::unique_ptr<lob::strategies::IStrategy>
make_strategy(const lob::utils::StrategyConfig &config) {
  const std::string_view name(config.name);
  if (name == "noop" || name == "no_op" || name == "none") {
    return std::make_unique<lob::strategies::NoopStrategy>();
  }
  if (is_fixed_spread_strategy(name)) {
    lob::strategies::FixedSpreadStrategyConfig strategy_config;
    strategy_config.delta_ticks = config.delta_ticks;
    strategy_config.order_quantity_lots = config.order_qty;
    strategy_config.max_inventory_lots = config.max_inventory;
    return std::make_unique<lob::strategies::FixedSpreadStrategy>(strategy_config);
  }
  if (is_avellaneda_stoikov_strategy(name)) {
    lob::strategies::AvellanedaStoikovStrategyConfig strategy_config;
    strategy_config.gamma = config.gamma;
    strategy_config.initial_sigma = config.sigma;
    strategy_config.k = config.k;
    strategy_config.horizon_seconds = config.horizon_seconds;
    strategy_config.sigma_window_ms = config.sigma_window_ms;
    strategy_config.min_spread_ticks = config.min_spread_ticks;
    strategy_config.order_quantity_lots = config.order_qty;
    strategy_config.max_inventory_lots = config.max_inventory;
    return std::make_unique<lob::strategies::AvellanedaStoikovStrategy>(strategy_config);
  }
  throw std::runtime_error("Strategy is not implemented yet: " + std::string(name));
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const std::filesystem::path config_path = parse_config_path(argc, argv);
    if (config_path.empty()) {
      return 0;
    }

    const lob::utils::AppConfig config = lob::utils::load_config(config_path);
    spdlog::info("Loaded LOB backtest config from {}", config_path.string());
    std::cout << lob::utils::describe_config(config) << '\n';

    lob::data::CsvDataSource source(lob::data::csv_config_from_directory(
        config.run.input_path, config.market.tick_size, config.market.lot_size));
    std::unique_ptr<lob::strategies::IStrategy> strategy = make_strategy(config.strategy);
    const lob::engine::BacktestResult result =
        lob::engine::BacktestEngine(engine_config_from_app_config(config)).run(source, *strategy);

    std::cout << "events=" << result.event_counts.total() << '\n';
    std::cout << "fills=" << result.fill_count << '\n';
    std::cout << "final_pnl=" << result.metrics.final_pnl << '\n';
    std::cout << "events_per_second=" << result.events_per_second << '\n';
    std::cout << "output_dir=" << config.run.output_dir.string() << '\n';
    return 0;
  } catch (const std::exception &error) {
    spdlog::error("{}", error.what());
    print_usage(std::cerr);
    return 1;
  }
}
