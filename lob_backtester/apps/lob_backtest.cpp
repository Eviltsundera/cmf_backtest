#include "lob/data/CsvDataSource.hpp"
#include "lob/engine/BacktestEngine.hpp"
#include "lob/strategies/Strategy.hpp"
#include "lob/utils/Config.hpp"
#include "lob/utils/GitMetadata.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

struct CliOptions {
  std::filesystem::path config_path;
  std::vector<lob::utils::ConfigOverride> overrides;
  bool json_output = false;
  bool help = false;
};

void print_usage(std::ostream &out) {
  out << "Usage: lob_backtest --config <path-to-yaml> [--override key=value ...] [--json]\n";
}

lob::utils::ConfigOverride parse_override(const std::string_view value) {
  const std::size_t delimiter = value.find('=');
  if (delimiter == std::string_view::npos || delimiter == 0 || delimiter + 1 == value.size()) {
    throw std::runtime_error("Invalid --override, expected key=value");
  }
  lob::utils::ConfigOverride parsed_override;
  parsed_override.key = std::string(value.substr(0, delimiter));
  parsed_override.value = std::string(value.substr(delimiter + 1));
  return parsed_override;
}

CliOptions parse_cli_options(const int argc, char **argv) {
  CliOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      options.help = true;
      return options;
    }
    if (arg == "--config" && index + 1 < argc) {
      options.config_path = std::filesystem::path(argv[++index]);
      continue;
    }
    if (arg == "--override" && index + 1 < argc) {
      options.overrides.push_back(parse_override(argv[++index]));
      continue;
    }
    if (arg.starts_with("--override=")) {
      options.overrides.push_back(
          parse_override(arg.substr(std::string_view("--override=").size())));
      continue;
    }
    if (arg == "--json") {
      options.json_output = true;
      continue;
    }
    throw std::runtime_error("Unknown argument: " + std::string(arg));
  }

  if (options.config_path.empty()) {
    throw std::runtime_error("Missing required --config <path-to-yaml> argument");
  }
  return options;
}

spdlog::level::level_enum parse_log_level(const std::string_view value) {
  if (value == "trace") {
    return spdlog::level::trace;
  }
  if (value == "debug") {
    return spdlog::level::debug;
  }
  if (value == "info") {
    return spdlog::level::info;
  }
  if (value == "warn" || value == "warning") {
    return spdlog::level::warn;
  }
  if (value == "error") {
    return spdlog::level::err;
  }
  if (value == "critical") {
    return spdlog::level::critical;
  }
  if (value == "off") {
    return spdlog::level::off;
  }
  throw std::runtime_error("Unsupported log_level: " + std::string(value));
}

void configure_logger() {
  spdlog::set_default_logger(spdlog::stderr_color_mt("lob_backtest"));
}

std::string json_escape(const std::string_view value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
            << static_cast<int>(c);
      } else {
        out << static_cast<char>(c);
      }
      break;
    }
  }
  return out.str();
}

void write_json_string(std::ostream &out, const std::string_view value) {
  out << '"' << json_escape(value) << '"';
}

void write_json_number(std::ostream &out, const double value) {
  if (std::isfinite(value)) {
    out << value;
  } else {
    out << "null";
  }
}

std::string git_commit_for_config(const std::filesystem::path &config_path) {
  if (const std::optional<std::string> commit = lob::utils::find_git_commit(config_path)) {
    return *commit;
  }
  if (const std::optional<std::string> commit =
          lob::utils::find_git_commit(std::filesystem::current_path())) {
    return *commit;
  }
  return "unknown";
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&now_time, &utc);

  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer.data();
}

void write_run_metadata_json(const std::filesystem::path &output_dir,
                             const std::filesystem::path &config_path,
                             const std::vector<lob::utils::ConfigOverride> &overrides,
                             const std::string_view hash, const std::string_view git_commit) {
  std::filesystem::create_directories(output_dir);
  const std::filesystem::path path = output_dir / "run_metadata.json";
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Unable to open run metadata: " + path.string());
  }

  out << "{\n";
  out << "  \"config_hash\": ";
  write_json_string(out, hash);
  out << ",\n  \"git_commit\": ";
  write_json_string(out, git_commit);
  out << ",\n  \"timestamp_utc\": ";
  write_json_string(out, utc_timestamp());
  out << ",\n  \"config_path\": ";
  write_json_string(out, config_path.string());
  out << ",\n  \"overrides\": [";
  for (std::size_t index = 0; index < overrides.size(); ++index) {
    const lob::utils::ConfigOverride &config_override = overrides[index];
    if (index > 0) {
      out << ',';
    }
    out << "\n    {\"key\": ";
    write_json_string(out, config_override.key);
    out << ", \"value\": ";
    write_json_string(out, config_override.value);
    out << '}';
  }
  if (!overrides.empty()) {
    out << '\n';
  }
  out << "  ]\n";
  out << "}\n";
}

void print_text_summary(const lob::utils::AppConfig &config,
                        const lob::engine::BacktestResult &result) {
  std::cout << lob::utils::describe_config(config) << '\n';
  std::cout << "events=" << result.event_counts.total() << '\n';
  std::cout << "fills=" << result.fill_count << '\n';
  std::cout << "final_pnl=" << result.metrics.final_pnl << '\n';
  std::cout << "events_per_second=" << result.events_per_second << '\n';
  std::cout << "output_dir=" << config.run.output_dir.string() << '\n';
}

void print_json_summary(const lob::utils::AppConfig &config,
                        const lob::engine::BacktestResult &result, const std::string_view hash,
                        const std::string_view git_commit) {
  std::cout << "{\n";
  std::cout << "  \"events\": " << result.event_counts.total() << ",\n";
  std::cout << "  \"fills\": " << result.fill_count << ",\n";
  std::cout << "  \"final_pnl\": ";
  write_json_number(std::cout, result.metrics.final_pnl);
  std::cout << ",\n  \"events_per_second\": ";
  write_json_number(std::cout, result.events_per_second);
  std::cout << ",\n  \"output_dir\": ";
  write_json_string(std::cout, config.run.output_dir.string());
  std::cout << ",\n  \"config_hash\": ";
  write_json_string(std::cout, hash);
  std::cout << ",\n  \"git_commit\": ";
  write_json_string(std::cout, git_commit);
  std::cout << "\n}\n";
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

bool is_microprice_as_strategy(const std::string_view name) {
  return name == "microprice_as" || name == "microprice_avellaneda_stoikov" || name == "mp_as";
}

bool is_avellaneda_stoikov_family(const std::string_view name) {
  return is_avellaneda_stoikov_strategy(name) || is_microprice_as_strategy(name);
}

lob::strategies::FairPriceMode parse_fair_price_mode(const std::string_view value) {
  if (value == "mid" || value == "classic") {
    return lob::strategies::FairPriceMode::Mid;
  }
  if (value == "microprice_proxy" || value == "microprice") {
    return lob::strategies::FairPriceMode::MicropriceProxy;
  }
  throw std::runtime_error("Unsupported fair_price_mode: " + std::string(value));
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
  engine_config.initial_cash = config.portfolio.initial_cash;
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
      is_avellaneda_stoikov_family(config.strategy.name)) {
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
  if (is_avellaneda_stoikov_family(name)) {
    if (is_microprice_as_strategy(name) && !config.has_fair_price_mode) {
      throw std::runtime_error("microprice_as strategy requires fair_price_mode");
    }

    lob::strategies::AvellanedaStoikovStrategyConfig strategy_config;
    strategy_config.gamma = config.gamma;
    strategy_config.initial_sigma = config.sigma;
    strategy_config.k = config.k;
    strategy_config.horizon_seconds = config.horizon_seconds;
    strategy_config.sigma_window_ms = config.sigma_window_ms;
    strategy_config.min_spread_ticks = config.min_spread_ticks;
    strategy_config.order_quantity_lots = config.order_qty;
    strategy_config.max_inventory_lots = config.max_inventory;
    strategy_config.fair_price_mode = parse_fair_price_mode(config.fair_price_mode);
    strategy_config.microprice_alpha = config.microprice_alpha;
    strategy_config.microprice_beta = config.microprice_beta;
    if (strategy_config.fair_price_mode == lob::strategies::FairPriceMode::MicropriceProxy) {
      if (!config.has_microprice_alpha || !config.has_microprice_beta) {
        throw std::runtime_error(
            "microprice fair_price_mode requires microprice_alpha and microprice_beta");
      }
    }
    if (is_microprice_as_strategy(name) &&
        strategy_config.fair_price_mode != lob::strategies::FairPriceMode::MicropriceProxy) {
      throw std::runtime_error("microprice_as strategy requires fair_price_mode: microprice_proxy");
    }
    return std::make_unique<lob::strategies::AvellanedaStoikovStrategy>(strategy_config);
  }
  throw std::runtime_error("Strategy is not implemented yet: " + std::string(name));
}

} // namespace

int main(const int argc, char **argv) {
  configure_logger();
  try {
    const CliOptions options = parse_cli_options(argc, argv);
    if (options.help) {
      print_usage(std::cout);
      return 0;
    }

    const lob::utils::AppConfig config = lob::utils::apply_overrides(
        lob::utils::load_config(options.config_path), options.overrides);
    spdlog::set_level(parse_log_level(config.run.log_level));
    const std::string effective_config_hash = lob::utils::config_hash(config);
    const std::string git_commit = git_commit_for_config(options.config_path);
    spdlog::info("Loaded LOB backtest config from {}", options.config_path.string());

    lob::data::CsvDataSource source(lob::data::csv_config_from_directory(
        config.run.input_path, config.market.tick_size, config.market.lot_size));
    std::unique_ptr<lob::strategies::IStrategy> strategy = make_strategy(config.strategy);
    const lob::engine::BacktestResult result =
        lob::engine::BacktestEngine(engine_config_from_app_config(config)).run(source, *strategy);
    write_run_metadata_json(config.run.output_dir, options.config_path, options.overrides,
                            effective_config_hash, git_commit);

    if (options.json_output) {
      print_json_summary(config, result, effective_config_hash, git_commit);
    } else {
      print_text_summary(config, result);
    }
    return 0;
  } catch (const std::exception &error) {
    spdlog::error("{}", error.what());
    print_usage(std::cerr);
    return 1;
  }
}
