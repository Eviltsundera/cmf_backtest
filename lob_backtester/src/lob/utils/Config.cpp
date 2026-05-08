#include "lob/utils/Config.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace lob::utils {
namespace {

YAML::Node require_section(const YAML::Node &root, const char *name) {
  const auto node = root[name];
  if (!node || !node.IsMap()) {
    throw std::runtime_error(std::string("Missing required config section: ") + name);
  }
  return node;
}

YAML::Node optional_section(const YAML::Node &root, const char *name) {
  const auto node = root[name];
  if (node && !node.IsMap()) {
    throw std::runtime_error(std::string("Config section must be a map: ") + name);
  }
  return node;
}

template <typename T> T required_value(const YAML::Node &node, const char *key) {
  const auto value = node[key];
  if (!value) {
    throw std::runtime_error(std::string("Missing required config key: ") + key);
  }
  return value.as<T>();
}

template <typename T>
T required_value_any(const YAML::Node &primary, const char *primary_key, const YAML::Node &fallback,
                     const char *fallback_key, const char *label) {
  if (primary) {
    if (const auto value = primary[primary_key]) {
      return value.as<T>();
    }
  }
  if (fallback) {
    if (const auto value = fallback[fallback_key]) {
      return value.as<T>();
    }
  }
  throw std::runtime_error(std::string("Missing required config key: ") + label);
}

template <typename T>
T optional_value_any(const YAML::Node &primary, const char *primary_key, const YAML::Node &fallback,
                     const char *fallback_key, const T default_value) {
  if (primary) {
    if (const auto value = primary[primary_key]) {
      return value.as<T>();
    }
  }
  if (fallback) {
    if (const auto value = fallback[fallback_key]) {
      return value.as<T>();
    }
  }
  return default_value;
}

template <typename T>
T optional_value(const YAML::Node &node, const char *key, const T default_value) {
  if (node) {
    if (const auto value = node[key]) {
      return value.as<T>();
    }
  }
  return default_value;
}

std::string_view trim(const std::string_view value) {
  std::string_view result = value;
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front())) != 0) {
    result.remove_prefix(1);
  }
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())) != 0) {
    result.remove_suffix(1);
  }
  return result;
}

std::runtime_error invalid_override(const std::string_view key, const std::string_view value,
                                    const std::string_view reason) {
  std::ostringstream out;
  out << "Invalid override " << key << '=' << value << ": " << reason;
  return std::runtime_error(out.str());
}

std::string parse_string_override(const std::string_view key, const std::string_view value) {
  const std::string_view trimmed = trim(value);
  if (trimmed.empty()) {
    throw invalid_override(key, value, "value must not be empty");
  }
  return std::string(trimmed);
}

double parse_double_override(const std::string_view key, const std::string_view value) {
  const std::string raw = parse_string_override(key, value);
  std::size_t parsed = 0;
  try {
    const double result = std::stod(raw, &parsed);
    if (parsed != raw.size()) {
      throw invalid_override(key, value, "expected a finite number");
    }
    if (!std::isfinite(result)) {
      throw invalid_override(key, value, "expected a finite number");
    }
    return result;
  } catch (const std::invalid_argument &) {
    throw invalid_override(key, value, "expected a finite number");
  } catch (const std::out_of_range &) {
    throw invalid_override(key, value, "number is out of range");
  }
}

std::int64_t parse_int64_override(const std::string_view key, const std::string_view value) {
  const std::string_view raw = trim(value);
  if (raw.empty()) {
    throw invalid_override(key, value, "value must not be empty");
  }
  std::int64_t result = 0;
  const auto *begin = raw.data();
  const auto *end = raw.data() + raw.size();
  const auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    throw invalid_override(key, value, "expected a signed integer");
  }
  return result;
}

std::uint32_t parse_uint32_override(const std::string_view key, const std::string_view value) {
  const std::string_view raw = trim(value);
  if (raw.empty()) {
    throw invalid_override(key, value, "value must not be empty");
  }
  if (raw.front() == '-') {
    throw invalid_override(key, value, "expected an unsigned integer");
  }
  std::uint64_t parsed_value = 0;
  const auto *begin = raw.data();
  const auto *end = raw.data() + raw.size();
  const auto parsed = std::from_chars(begin, end, parsed_value);
  if (parsed.ec != std::errc{} || parsed.ptr != end ||
      parsed_value > std::numeric_limits<std::uint32_t>::max()) {
    throw invalid_override(key, value, "expected a uint32 value");
  }
  return static_cast<std::uint32_t>(parsed_value);
}

bool parse_bool_override(const std::string_view key, const std::string_view value) {
  const std::string raw = parse_string_override(key, value);
  std::string lowered;
  lowered.reserve(raw.size());
  for (const char c : raw) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  if (lowered == "true" || lowered == "1") {
    return true;
  }
  if (lowered == "false" || lowered == "0") {
    return false;
  }
  throw invalid_override(key, value, "expected true/false");
}

void apply_override(AppConfig &config, const ConfigOverride &override) {
  const std::string_view key = trim(override.key);
  const std::string_view value = trim(override.value);

  if (key == "run.symbol") {
    config.run.symbol = parse_string_override(key, value);
  } else if (key == "run.input_path" || key == "data.path") {
    config.run.input_path = parse_string_override(key, value);
  } else if (key == "run.output_dir") {
    config.run.output_dir = parse_string_override(key, value);
  } else if (key == "run.log_level") {
    config.run.log_level = parse_string_override(key, value);
  } else if (key == "market.tick_size" || key == "data.tick_size") {
    config.market.tick_size = parse_double_override(key, value);
  } else if (key == "market.lot_size" || key == "data.lot_size") {
    config.market.lot_size = parse_double_override(key, value);
  } else if (key == "book.max_depth" || key == "data.max_depth") {
    config.book.max_depth = parse_uint32_override(key, value);
  } else if (key == "portfolio.initial_cash") {
    config.portfolio.initial_cash = parse_double_override(key, value);
  } else if (key == "portfolio.max_inventory") {
    config.strategy.max_inventory = parse_int64_override(key, value);
  } else if (key == "execution.fill_model") {
    config.execution.fill_model = parse_string_override(key, value);
  } else if (key == "execution.fill_reference") {
    config.execution.fill_reference = parse_string_override(key, value);
  } else if (key == "execution.partial_fills") {
    config.execution.partial_fills = parse_bool_override(key, value);
  } else if (key == "execution.maker_bps" || key == "fees.maker_bps") {
    config.execution.maker_bps = parse_double_override(key, value);
  } else if (key == "execution.taker_bps" || key == "fees.taker_bps") {
    config.execution.taker_bps = parse_double_override(key, value);
  } else if (key == "strategy.name") {
    config.strategy.name = parse_string_override(key, value);
  } else if (key == "strategy.gamma") {
    config.strategy.gamma = parse_double_override(key, value);
  } else if (key == "strategy.sigma") {
    config.strategy.sigma = parse_double_override(key, value);
  } else if (key == "strategy.k") {
    config.strategy.k = parse_double_override(key, value);
  } else if (key == "strategy.horizon_seconds") {
    config.strategy.horizon_seconds = parse_double_override(key, value);
  } else if (key == "strategy.sigma_window_ms") {
    config.strategy.sigma_window_ms = parse_int64_override(key, value);
  } else if (key == "strategy.min_spread_ticks") {
    config.strategy.min_spread_ticks = parse_int64_override(key, value);
  } else if (key == "strategy.fair_price_mode") {
    config.strategy.fair_price_mode = parse_string_override(key, value);
    config.strategy.has_fair_price_mode = true;
  } else if (key == "strategy.microprice_alpha") {
    config.strategy.microprice_alpha = parse_double_override(key, value);
    config.strategy.has_microprice_alpha = true;
  } else if (key == "strategy.microprice_beta") {
    config.strategy.microprice_beta = parse_double_override(key, value);
    config.strategy.has_microprice_beta = true;
  } else if (key == "strategy.delta_ticks") {
    config.strategy.delta_ticks = parse_int64_override(key, value);
  } else if (key == "strategy.order_qty") {
    config.strategy.order_qty = parse_int64_override(key, value);
  } else if (key == "strategy.max_inventory") {
    config.strategy.max_inventory = parse_int64_override(key, value);
  } else if (key == "strategy.quote_refresh_ms" || key == "engine.quote_refresh_ms") {
    config.strategy.quote_refresh_ms = parse_int64_override(key, value);
  } else {
    throw std::runtime_error("Unknown config override key: " + std::string(key));
  }
}

} // namespace

AppConfig load_config(const std::filesystem::path &path) {
  const YAML::Node root = YAML::LoadFile(path.string());

  const YAML::Node run = require_section(root, "run");
  const YAML::Node data = optional_section(root, "data");
  const YAML::Node market = optional_section(root, "market");
  const YAML::Node book = optional_section(root, "book");
  const YAML::Node engine = optional_section(root, "engine");
  const YAML::Node execution = require_section(root, "execution");
  const YAML::Node fees = optional_section(root, "fees");
  const YAML::Node portfolio = optional_section(root, "portfolio");
  const YAML::Node strategy = require_section(root, "strategy");

  AppConfig config;
  config.run.symbol = required_value<std::string>(run, "symbol");
  config.run.input_path =
      required_value_any<std::string>(run, "input_path", data, "path", "run.input_path");
  config.run.output_dir = required_value<std::string>(run, "output_dir");
  config.run.log_level = run["log_level"] ? run["log_level"].as<std::string>() : "info";

  config.market.tick_size =
      required_value_any<double>(market, "tick_size", data, "tick_size", "market.tick_size");
  config.market.lot_size =
      required_value_any<double>(market, "lot_size", data, "lot_size", "market.lot_size");

  config.book.max_depth =
      required_value_any<std::uint32_t>(book, "max_depth", data, "max_depth", "book.max_depth");

  config.portfolio.initial_cash = optional_value<double>(portfolio, "initial_cash", 0.0);

  config.execution.fill_model = required_value<std::string>(execution, "fill_model");
  config.execution.fill_reference = required_value<std::string>(execution, "fill_reference");
  config.execution.partial_fills = required_value<bool>(execution, "partial_fills");
  config.execution.maker_bps =
      optional_value_any<double>(execution, "maker_bps", fees, "maker_bps", 0.0);
  config.execution.taker_bps =
      optional_value_any<double>(execution, "taker_bps", fees, "taker_bps", 0.0);

  config.strategy.name = required_value<std::string>(strategy, "name");
  config.strategy.gamma = strategy["gamma"] ? strategy["gamma"].as<double>() : 0.0;
  config.strategy.sigma = strategy["sigma"] ? strategy["sigma"].as<double>() : 0.0;
  config.strategy.k = strategy["k"] ? strategy["k"].as<double>() : 0.0;
  config.strategy.horizon_seconds =
      strategy["horizon_seconds"] ? strategy["horizon_seconds"].as<double>() : 0.0;
  config.strategy.sigma_window_ms =
      strategy["sigma_window_ms"] ? strategy["sigma_window_ms"].as<std::int64_t>() : 0;
  config.strategy.min_spread_ticks =
      strategy["min_spread_ticks"] ? strategy["min_spread_ticks"].as<std::int64_t>() : 0;
  config.strategy.has_fair_price_mode = static_cast<bool>(strategy["fair_price_mode"]);
  config.strategy.fair_price_mode =
      strategy["fair_price_mode"] ? strategy["fair_price_mode"].as<std::string>() : "mid";
  config.strategy.has_microprice_alpha = static_cast<bool>(strategy["microprice_alpha"]);
  config.strategy.microprice_alpha =
      strategy["microprice_alpha"] ? strategy["microprice_alpha"].as<double>() : 0.0;
  config.strategy.has_microprice_beta = static_cast<bool>(strategy["microprice_beta"]);
  config.strategy.microprice_beta =
      strategy["microprice_beta"] ? strategy["microprice_beta"].as<double>() : 0.0;
  config.strategy.delta_ticks =
      strategy["delta_ticks"] ? strategy["delta_ticks"].as<std::int64_t>() : 0;
  config.strategy.order_qty = strategy["order_qty"] ? strategy["order_qty"].as<std::int64_t>() : 0;
  config.strategy.max_inventory =
      optional_value_any<std::int64_t>(strategy, "max_inventory", portfolio, "max_inventory", 0);
  config.strategy.quote_refresh_ms =
      optional_value_any<std::int64_t>(strategy, "quote_refresh_ms", engine, "quote_refresh_ms", 0);

  return config;
}

AppConfig apply_overrides(AppConfig config, const std::vector<ConfigOverride> &overrides) {
  for (const ConfigOverride &override : overrides) {
    apply_override(config, override);
  }
  return config;
}

std::string describe_config(const AppConfig &config) {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "symbol=" << config.run.symbol << '\n';
  out << "input_path=" << config.run.input_path.string() << '\n';
  out << "output_dir=" << config.run.output_dir.string() << '\n';
  out << "log_level=" << config.run.log_level << '\n';
  out << "tick_size=" << config.market.tick_size << '\n';
  out << "lot_size=" << config.market.lot_size << '\n';
  out << "max_depth=" << config.book.max_depth << '\n';
  out << "initial_cash=" << config.portfolio.initial_cash << '\n';
  out << "fill_model=" << config.execution.fill_model << '\n';
  out << "fill_reference=" << config.execution.fill_reference << '\n';
  out << "partial_fills=" << (config.execution.partial_fills ? "true" : "false") << '\n';
  out << "maker_bps=" << config.execution.maker_bps << '\n';
  out << "taker_bps=" << config.execution.taker_bps << '\n';
  out << "strategy=" << config.strategy.name << '\n';
  out << "gamma=" << config.strategy.gamma << '\n';
  out << "sigma=" << config.strategy.sigma << '\n';
  out << "k=" << config.strategy.k << '\n';
  out << "horizon_seconds=" << config.strategy.horizon_seconds << '\n';
  out << "sigma_window_ms=" << config.strategy.sigma_window_ms << '\n';
  out << "min_spread_ticks=" << config.strategy.min_spread_ticks << '\n';
  out << "fair_price_mode=" << config.strategy.fair_price_mode << '\n';
  out << "microprice_alpha=" << config.strategy.microprice_alpha << '\n';
  out << "microprice_beta=" << config.strategy.microprice_beta << '\n';
  out << "delta_ticks=" << config.strategy.delta_ticks << '\n';
  out << "order_qty=" << config.strategy.order_qty << '\n';
  out << "max_inventory=" << config.strategy.max_inventory << '\n';
  out << "quote_refresh_ms=" << config.strategy.quote_refresh_ms;
  return out.str();
}

std::string config_hash(const AppConfig &config) {
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

  std::uint64_t hash = kFnvOffset;
  const std::string serialized = describe_config(config);
  for (const unsigned char byte : serialized) {
    hash ^= byte;
    hash *= kFnvPrime;
  }

  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

} // namespace lob::utils
