#include "lob/utils/Config.hpp"

#include <sstream>
#include <stdexcept>

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

template <typename T> T required_value(const YAML::Node &node, const char *key) {
  const auto value = node[key];
  if (!value) {
    throw std::runtime_error(std::string("Missing required config key: ") + key);
  }
  return value.as<T>();
}

} // namespace

AppConfig load_config(const std::filesystem::path &path) {
  const YAML::Node root = YAML::LoadFile(path.string());

  const YAML::Node run = require_section(root, "run");
  const YAML::Node market = require_section(root, "market");
  const YAML::Node book = require_section(root, "book");
  const YAML::Node execution = require_section(root, "execution");
  const YAML::Node strategy = require_section(root, "strategy");

  AppConfig config;
  config.run.symbol = required_value<std::string>(run, "symbol");
  config.run.input_path = required_value<std::string>(run, "input_path");
  config.run.output_dir = required_value<std::string>(run, "output_dir");

  config.market.tick_size = required_value<double>(market, "tick_size");
  config.market.lot_size = required_value<double>(market, "lot_size");

  config.book.max_depth = required_value<std::uint32_t>(book, "max_depth");

  config.execution.fill_model = required_value<std::string>(execution, "fill_model");
  config.execution.fill_reference = required_value<std::string>(execution, "fill_reference");
  config.execution.partial_fills = required_value<bool>(execution, "partial_fills");
  config.execution.maker_bps = execution["maker_bps"] ? execution["maker_bps"].as<double>() : 0.0;
  config.execution.taker_bps = execution["taker_bps"] ? execution["taker_bps"].as<double>() : 0.0;

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
      strategy["max_inventory"] ? strategy["max_inventory"].as<std::int64_t>() : 0;
  config.strategy.quote_refresh_ms =
      strategy["quote_refresh_ms"] ? strategy["quote_refresh_ms"].as<std::int64_t>() : 0;

  return config;
}

std::string describe_config(const AppConfig &config) {
  std::ostringstream out;
  out << "symbol=" << config.run.symbol << '\n';
  out << "input_path=" << config.run.input_path.string() << '\n';
  out << "output_dir=" << config.run.output_dir.string() << '\n';
  out << "tick_size=" << config.market.tick_size << '\n';
  out << "lot_size=" << config.market.lot_size << '\n';
  out << "max_depth=" << config.book.max_depth << '\n';
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

} // namespace lob::utils
