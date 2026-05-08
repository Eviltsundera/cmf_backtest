#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lob::utils {

struct RunConfig {
  std::string symbol;
  std::filesystem::path input_path;
  std::filesystem::path output_dir;
  std::string log_level = "info";
};

struct MarketConfig {
  double tick_size = 0.0;
  double lot_size = 0.0;
};

struct BookConfig {
  std::uint32_t max_depth = 0;
};

struct PortfolioConfig {
  double initial_cash = 0.0;
};

struct ExecutionConfig {
  std::string fill_model;
  std::string fill_reference;
  bool partial_fills = false;
  double maker_bps = 0.0;
  double taker_bps = 0.0;
};

struct StrategyConfig {
  std::string name;
  double gamma = 0.0;
  double sigma = 0.0;
  double k = 0.0;
  double horizon_seconds = 0.0;
  std::int64_t sigma_window_ms = 0;
  std::int64_t min_spread_ticks = 0;
  std::string fair_price_mode = "mid";
  bool has_fair_price_mode = false;
  double microprice_alpha = 0.0;
  bool has_microprice_alpha = false;
  double microprice_beta = 0.0;
  bool has_microprice_beta = false;
  std::int64_t delta_ticks = 0;
  std::int64_t order_qty = 0;
  std::int64_t max_inventory = 0;
  std::int64_t quote_refresh_ms = 0;
};

struct AppConfig {
  RunConfig run;
  MarketConfig market;
  BookConfig book;
  PortfolioConfig portfolio;
  ExecutionConfig execution;
  StrategyConfig strategy;
};

struct ConfigOverride {
  std::string key;
  std::string value;
};

AppConfig load_config(const std::filesystem::path &path);

AppConfig apply_overrides(AppConfig config, const std::vector<ConfigOverride> &overrides);

std::string describe_config(const AppConfig &config);

std::string config_hash(const AppConfig &config);

} // namespace lob::utils
