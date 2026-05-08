#include "lob/features/FeatureEngine.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

lob::book::OrderBook make_book(const std::int64_t bid_price, const std::int64_t bid_quantity,
                               const std::int64_t ask_price, const std::int64_t ask_quantity) {
  lob::book::OrderBook book;
  if (bid_quantity >= 0) {
    book.apply_update(lob::data::BookSide::Bid, bid_price, bid_quantity);
  }
  if (ask_quantity >= 0) {
    book.apply_update(lob::data::BookSide::Ask, ask_price, ask_quantity);
  }
  return book;
}

double numpy_population_std(const std::vector<double> &values) {
  double sum = 0.0;
  for (const double value : values) {
    sum += value;
  }
  const double mean = sum / static_cast<double>(values.size());

  double squared_error_sum = 0.0;
  for (const double value : values) {
    const double delta = value - mean;
    squared_error_sum += delta * delta;
  }

  return std::sqrt(squared_error_sum / static_cast<double>(values.size()));
}

void expect_optional_near(const std::optional<double> value, const double expected) {
  ASSERT_TRUE(value.has_value());
  EXPECT_NEAR(*value, expected, 1e-12);
}

} // namespace

TEST(FeatureEngineTest, ComputesTopOfBookFeatures) {
  const auto book = make_book(100, 30, 110, 10);

  expect_optional_near(lob::features::mid(book), 105.0);
  expect_optional_near(lob::features::spread(book), 10.0);
  expect_optional_near(lob::features::imbalance(book), 0.5);
  expect_optional_near(lob::features::weighted_mid(book), 107.5);
  expect_optional_near(lob::features::microprice_proxy(book, 1.0), 107.5);
}

TEST(FeatureEngineTest, ReturnsEmptyFeaturesWhenOneSideIsMissing) {
  lob::book::OrderBook book;
  book.apply_update(lob::data::BookSide::Bid, 100, 10);

  EXPECT_FALSE(lob::features::mid(book).has_value());
  EXPECT_FALSE(lob::features::spread(book).has_value());
  EXPECT_FALSE(lob::features::imbalance(book).has_value());
  EXPECT_FALSE(lob::features::weighted_mid(book).has_value());
  EXPECT_FALSE(lob::features::microprice_proxy(book, 1.0).has_value());
}

TEST(FeatureEngineTest, ZeroQuantityUpdateRemovesTopLevel) {
  const auto book = make_book(100, 0, 110, 0);

  EXPECT_FALSE(lob::features::mid(book).has_value());
  EXPECT_FALSE(lob::features::spread(book).has_value());
  EXPECT_FALSE(lob::features::imbalance(book).has_value());
  EXPECT_FALSE(lob::features::weighted_mid(book).has_value());
  EXPECT_FALSE(lob::features::microprice_proxy(book, 1.0).has_value());
}

TEST(FeatureEngineTest, MicropriceAlphaZeroEqualsMid) {
  const auto book = make_book(100, 30, 110, 10);

  expect_optional_near(lob::features::microprice_proxy(book, 0.0), *lob::features::mid(book));
}

TEST(FeatureEngineTest, MicropriceWithBalancedVolumesEqualsMid) {
  const auto book = make_book(100, 10, 110, 10);

  expect_optional_near(lob::features::imbalance(book), 0.0);
  expect_optional_near(lob::features::microprice_proxy(book, 1.0), *lob::features::mid(book));
}

TEST(RollingStdTest, MatchesNumpyPopulationStdOnSyntheticReturns) {
  const std::vector<double> returns{0.01, -0.02, 0.03, 0.015, -0.005};
  lob::features::RollingStd rolling_std(3);

  EXPECT_NEAR(rolling_std.push(returns[0]), 0.0, 1e-12);
  EXPECT_NEAR(rolling_std.push(returns[1]), numpy_population_std({returns[0], returns[1]}), 1e-12);
  EXPECT_NEAR(rolling_std.push(returns[2]),
              numpy_population_std({returns[0], returns[1], returns[2]}), 1e-12);
  EXPECT_NEAR(rolling_std.push(returns[3]),
              numpy_population_std({returns[1], returns[2], returns[3]}), 1e-12);
  EXPECT_NEAR(rolling_std.push(returns[4]),
              numpy_population_std({returns[2], returns[3], returns[4]}), 1e-12);
  EXPECT_EQ(rolling_std.count(), 3U);
}

TEST(RollingMidReturnStdTest, ComputesSimpleMidReturnVolatility) {
  lob::features::RollingMidReturnStd rolling_std(3);

  EXPECT_FALSE(rolling_std.push_mid(100.0).has_value());
  expect_optional_near(rolling_std.push_mid(101.0), 0.0);
  expect_optional_near(rolling_std.push_mid(99.0),
                       numpy_population_std({0.01, (99.0 - 101.0) / 101.0}));
  EXPECT_EQ(rolling_std.count(), 2U);
}

TEST(RollingStdTest, RejectsInvalidInputs) {
  EXPECT_THROW(lob::features::RollingStd(0), std::runtime_error);

  lob::features::RollingMidReturnStd rolling_std(2);
  EXPECT_THROW(rolling_std.push_mid(0.0), std::runtime_error);
}
