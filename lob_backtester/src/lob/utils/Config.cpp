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
  config.strategy.gamma = required_value<double>(strategy, "gamma");
  config.strategy.sigma = required_value<double>(strategy, "sigma");
  config.strategy.k = required_value<double>(strategy, "k");

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
  out << "k=" << config.strategy.k;
  return out.str();
}

} // namespace lob::utils
