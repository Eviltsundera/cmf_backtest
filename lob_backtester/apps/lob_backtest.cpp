#include "lob/utils/Config.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
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
    return 0;
  } catch (const std::exception &error) {
    spdlog::error("{}", error.what());
    print_usage(std::cerr);
    return 1;
  }
}
