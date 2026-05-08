#include "lob/utils/Config.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <gtest/gtest.h>

TEST(ConfigSmokeTest, LoadsExampleConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "example.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.run.symbol, "MD");
  EXPECT_EQ(config.run.log_level, "info");
  EXPECT_EQ(config.execution.fill_model, "price_cross");
  EXPECT_FALSE(config.execution.partial_fills);
  EXPECT_GT(config.market.tick_size, 0.0);
  EXPECT_GT(config.book.max_depth, 0U);
  EXPECT_DOUBLE_EQ(config.portfolio.initial_cash, 0.0);
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

TEST(ConfigSmokeTest, LoadsMicropriceAsConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "microprice_as.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.strategy.name, "microprice_as");
  EXPECT_EQ(config.strategy.fair_price_mode, "microprice_proxy");
  EXPECT_TRUE(config.strategy.has_fair_price_mode);
  EXPECT_TRUE(config.strategy.has_microprice_alpha);
  EXPECT_TRUE(config.strategy.has_microprice_beta);
  EXPECT_GT(config.strategy.microprice_alpha, 0.0);
  EXPECT_GT(config.strategy.microprice_beta, 0.0);
  EXPECT_EQ(config.strategy.order_qty, 1);
  EXPECT_EQ(config.strategy.max_inventory, 10);
  EXPECT_EQ(config.run.output_dir,
            std::filesystem::path("lob_backtester/artifacts/runs/microprice_as"));
}

TEST(ConfigSmokeTest, AppliesOverridesToEffectiveConfig) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "avellaneda_stoikov.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  const lob::utils::AppConfig overridden = lob::utils::apply_overrides(
      config, {{"strategy.gamma", "0.05"},
               {"run.output_dir", "lob_backtester/artifacts/runs/override"},
               {"execution.partial_fills", "true"},
               {"portfolio.initial_cash", "100000.5"},
               {"fees.maker_bps", "-1.25"}});

  EXPECT_DOUBLE_EQ(overridden.strategy.gamma, 0.05);
  EXPECT_EQ(overridden.run.output_dir,
            std::filesystem::path("lob_backtester/artifacts/runs/override"));
  EXPECT_TRUE(overridden.execution.partial_fills);
  EXPECT_DOUBLE_EQ(overridden.portfolio.initial_cash, 100000.5);
  EXPECT_DOUBLE_EQ(overridden.execution.maker_bps, -1.25);
  EXPECT_NE(lob::utils::config_hash(config), lob::utils::config_hash(overridden));
}

TEST(ConfigSmokeTest, RejectsInvalidOverrideWithoutMutatingOriginal) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "avellaneda_stoikov.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_THROW(static_cast<void>(lob::utils::apply_overrides(
                   config, {{"strategy.gamma", "0.05"}, {"strategy.sigma", "nan"}})),
               std::runtime_error);
  EXPECT_DOUBLE_EQ(config.strategy.gamma, 0.01);
  EXPECT_DOUBLE_EQ(config.strategy.sigma, 1.0);
}

TEST(ConfigSmokeTest, MarksMicropriceFieldsPresentWhenOverridden) {
  const auto path = std::filesystem::path(LOB_TEST_CONFIG_DIR) / "avellaneda_stoikov.yaml";
  const lob::utils::AppConfig config = lob::utils::load_config(path);

  const lob::utils::AppConfig overridden =
      lob::utils::apply_overrides(config, {{"strategy.fair_price_mode", "microprice_proxy"},
                                           {"strategy.microprice_alpha", "0.5"},
                                           {"strategy.microprice_beta", "0.25"}});

  EXPECT_TRUE(overridden.strategy.has_fair_price_mode);
  EXPECT_TRUE(overridden.strategy.has_microprice_alpha);
  EXPECT_TRUE(overridden.strategy.has_microprice_beta);
  EXPECT_EQ(overridden.strategy.fair_price_mode, "microprice_proxy");
  EXPECT_DOUBLE_EQ(overridden.strategy.microprice_alpha, 0.5);
  EXPECT_DOUBLE_EQ(overridden.strategy.microprice_beta, 0.25);
}

TEST(ConfigSmokeTest, LoadsPlanStyleAliasSections) {
  const auto path = std::filesystem::temp_directory_path() / "lob_plan_style_config.yaml";
  {
    std::ofstream out(path);
    out << R"(run:
  symbol: MD
  output_dir: lob_backtester/artifacts/runs/plan_style
  log_level: warn

data:
  path: data/sample
  tick_size: 0.0000001
  lot_size: 1.0
  max_depth: 20

engine:
  quote_refresh_ms: 250

execution:
  fill_model: price_cross
  fill_reference: trade_price
  partial_fills: false

fees:
  maker_bps: -0.5
  taker_bps: 1.25

portfolio:
  initial_cash: 100000.0
  max_inventory: 7

strategy:
  name: fixed_spread
  delta_ticks: 1
  order_qty: 1
)";
  }

  const lob::utils::AppConfig config = lob::utils::load_config(path);

  EXPECT_EQ(config.run.input_path, std::filesystem::path("data/sample"));
  EXPECT_EQ(config.run.log_level, "warn");
  EXPECT_DOUBLE_EQ(config.market.tick_size, 0.0000001);
  EXPECT_DOUBLE_EQ(config.market.lot_size, 1.0);
  EXPECT_EQ(config.book.max_depth, 20U);
  EXPECT_EQ(config.strategy.quote_refresh_ms, 250);
  EXPECT_DOUBLE_EQ(config.execution.maker_bps, -0.5);
  EXPECT_DOUBLE_EQ(config.execution.taker_bps, 1.25);
  EXPECT_DOUBLE_EQ(config.portfolio.initial_cash, 100000.0);
  EXPECT_EQ(config.strategy.max_inventory, 7);
  std::filesystem::remove(path);
}
