#include "lob/utils/Config.hpp"

#include <filesystem>

#include <gtest/gtest.h>

TEST(ConfigSmokeTest, LoadsExampleConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "example.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.run.symbol, "MD");
  EXPECT_EQ(config.execution.fill_model, "price_cross");
  EXPECT_FALSE(config.execution.partial_fills);
  EXPECT_GT(config.market.tick_size, 0.0);
  EXPECT_GT(config.book.max_depth, 0U);
}

TEST(ConfigSmokeTest, LoadsBaselineFixedSpreadConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "baseline_fixed_spread.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.strategy.name, "fixed_spread");
  EXPECT_EQ(config.strategy.delta_ticks, 1);
  EXPECT_EQ(config.strategy.order_qty, 1);
  EXPECT_EQ(config.strategy.max_inventory, 10);
  EXPECT_EQ(config.run.output_dir,
            std::filesystem::path("lob_backtester/artifacts/runs/baseline_fixed"));
}

TEST(ConfigSmokeTest, LoadsAvellanedaStoikovConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "avellaneda_stoikov.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.strategy.name, "avellaneda_stoikov");
  EXPECT_GT(config.strategy.gamma, 0.0);
  EXPECT_GE(config.strategy.sigma, 0.0);
  EXPECT_GT(config.strategy.k, 0.0);
  EXPECT_GT(config.strategy.horizon_seconds, 0.0);
  EXPECT_GT(config.strategy.sigma_window_ms, 0);
  EXPECT_EQ(config.strategy.min_spread_ticks, 2);
  EXPECT_EQ(config.strategy.order_qty, 1);
  EXPECT_EQ(config.strategy.max_inventory, 10);
  EXPECT_EQ(config.run.output_dir,
            std::filesystem::path("lob_backtester/artifacts/runs/avellaneda_stoikov"));
}
