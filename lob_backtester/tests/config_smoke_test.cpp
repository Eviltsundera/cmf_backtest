#include "lob/utils/Config.hpp"

#include <filesystem>

#include <gtest/gtest.h>

TEST(ConfigSmokeTest, LoadsExampleConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "example.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.run.symbol, "BTCUSDT");
  EXPECT_EQ(config.execution.fill_model, "price_cross");
  EXPECT_FALSE(config.execution.partial_fills);
  EXPECT_GT(config.market.tick_size, 0.0);
  EXPECT_GT(config.book.max_depth, 0U);
}
