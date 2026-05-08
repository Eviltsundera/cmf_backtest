#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace lob::utils {

struct RunConfig {
  std::string symbol;
  std::filesystem::path input_path;
  std::filesystem::path output_dir;
};

struct MarketConfig {
  double tick_size = 0.0;
  double lot_size = 0.0;
};

struct BookConfig {
  std::uint32_t max_depth = 0;
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
};

struct AppConfig {
  RunConfig run;
  MarketConfig market;
  BookConfig book;
  ExecutionConfig execution;
  StrategyConfig strategy;
};

AppConfig load_config(const std::filesystem::path &path);

std::string describe_config(const AppConfig &config);

} // namespace lob::utils
