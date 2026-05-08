#pragma once

#include "lob/data/MarketEvent.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace lob::book {

using Price = std::int64_t;
using Quantity = std::int64_t;

struct BookLevel {
  Price price_ticks = 0;
  Quantity quantity_lots = 0;
};

enum class CrossedBookPolicy : std::uint8_t {
  Reject = 0,
  DropCrossingLevels = 1,
  IgnoreWithWarning = 2,
};

struct OrderBookConfig {
  std::uint32_t max_depth = 0;
  CrossedBookPolicy crossed_book_policy = CrossedBookPolicy::DropCrossingLevels;
};

class OrderBook {
public:
  explicit OrderBook(OrderBookConfig config = {});

  void apply_snapshot(const data::SnapshotPayload &snapshot);
  void apply_update(data::BookSide side, Price price_ticks, Quantity quantity_lots);
  bool apply_event(const data::MarketEvent &event);

  [[nodiscard]] std::optional<BookLevel> best_bid() const;
  [[nodiscard]] std::optional<BookLevel> best_ask() const;
  [[nodiscard]] std::optional<double> mid() const;
  [[nodiscard]] std::optional<Price> spread() const;
  [[nodiscard]] std::optional<BookLevel> level(data::BookSide side, std::size_t depth) const;

  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t bid_depth() const;
  [[nodiscard]] std::size_t ask_depth() const;
  [[nodiscard]] bool is_crossed_or_locked() const;
  [[nodiscard]] std::uint64_t snapshot_id() const;

private:
  using BidLevels = std::map<Price, Quantity, std::greater<Price>>;
  using AskLevels = std::map<Price, Quantity>;

  void set_level(data::BookSide side, Price price_ticks, Quantity quantity_lots);
  void restore_level(data::BookSide side, Price price_ticks, std::optional<Quantity> quantity);
  void trim_to_max_depth();
  void enforce_crossed_policy(std::optional<data::BookSide> changed_side);
  void recover_crossed_book(std::optional<data::BookSide> changed_side);

  OrderBookConfig config_;
  BidLevels bids_;
  AskLevels asks_;
  std::uint64_t snapshot_id_ = 0;
};

} // namespace lob::book
