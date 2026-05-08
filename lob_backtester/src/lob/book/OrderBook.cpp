#include "lob/book/OrderBook.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

namespace lob::book {
namespace {

template <typename Map> void trim_levels(Map &levels, const std::uint32_t max_depth) {
  if (max_depth == 0) {
    return;
  }

  while (levels.size() > max_depth) {
    levels.erase(std::prev(levels.end()));
  }
}

template <typename BidLevels, typename AskLevels>
bool crossed_or_locked(const BidLevels &bids, const AskLevels &asks) {
  return !bids.empty() && !asks.empty() && bids.begin()->first >= asks.begin()->first;
}

template <typename Iterator> BookLevel to_book_level(const Iterator it) {
  return BookLevel{.price_ticks = it->first, .quantity_lots = it->second};
}

void validate_level(const Price price_ticks, const Quantity quantity_lots) {
  if (price_ticks <= 0) {
    throw std::runtime_error("OrderBook price must be positive");
  }
  if (quantity_lots < 0) {
    throw std::runtime_error("OrderBook quantity must be non-negative");
  }
}

} // namespace

OrderBook::OrderBook(OrderBookConfig config) : config_(config) {
}

void OrderBook::apply_snapshot(const data::SnapshotPayload &snapshot) {
  BidLevels next_bids;
  AskLevels next_asks;

  const auto depth =
      std::min<std::size_t>(snapshot.depth, static_cast<std::size_t>(data::kSnapshotDepth));
  for (std::size_t index = 0; index < depth; ++index) {
    const data::PriceLevel &ask = snapshot.asks[index];
    if (ask.quantity_lots < 0) {
      throw std::runtime_error("OrderBook snapshot ask quantity must be non-negative");
    }
    if (ask.quantity_lots > 0) {
      validate_level(ask.price_ticks, ask.quantity_lots);
      next_asks[ask.price_ticks] = ask.quantity_lots;
    }

    const data::PriceLevel &bid = snapshot.bids[index];
    if (bid.quantity_lots < 0) {
      throw std::runtime_error("OrderBook snapshot bid quantity must be non-negative");
    }
    if (bid.quantity_lots > 0) {
      validate_level(bid.price_ticks, bid.quantity_lots);
      next_bids[bid.price_ticks] = bid.quantity_lots;
    }
  }

  if (config_.crossed_book_policy == CrossedBookPolicy::Reject &&
      crossed_or_locked(next_bids, next_asks)) {
    throw std::runtime_error("OrderBook snapshot is crossed or locked");
  }

  bids_ = std::move(next_bids);
  asks_ = std::move(next_asks);
  enforce_crossed_policy(std::nullopt);
  trim_to_max_depth();
  ++snapshot_id_;
}

void OrderBook::apply_update(const data::BookSide side, const Price price_ticks,
                             const Quantity quantity_lots) {
  std::optional<BidLevels> previous_bids;
  std::optional<AskLevels> previous_asks;
  if (config_.crossed_book_policy == CrossedBookPolicy::Reject) {
    previous_bids = bids_;
    previous_asks = asks_;
  }

  std::optional<Quantity> previous_quantity;
  if (side == data::BookSide::Bid) {
    const auto it = bids_.find(price_ticks);
    if (it != bids_.end()) {
      previous_quantity = it->second;
    }
  } else {
    const auto it = asks_.find(price_ticks);
    if (it != asks_.end()) {
      previous_quantity = it->second;
    }
  }

  set_level(side, price_ticks, quantity_lots);

  try {
    enforce_crossed_policy(side);
    trim_to_max_depth();
  } catch (...) {
    if (previous_bids && previous_asks) {
      bids_ = std::move(*previous_bids);
      asks_ = std::move(*previous_asks);
    } else {
      restore_level(side, price_ticks, previous_quantity);
    }
    throw;
  }
}

bool OrderBook::apply_event(const data::MarketEvent &event) {
  switch (event.type) {
  case data::EventType::Snapshot:
    apply_snapshot(event.payload.snapshot);
    return true;
  case data::EventType::DepthUpdate:
    apply_update(event.payload.depth_update.side, event.payload.depth_update.price_ticks,
                 event.payload.depth_update.quantity_lots);
    return true;
  case data::EventType::Trade:
    return false;
  }

  return false;
}

std::optional<BookLevel> OrderBook::best_bid() const {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return to_book_level(bids_.begin());
}

std::optional<BookLevel> OrderBook::best_ask() const {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return to_book_level(asks_.begin());
}

std::optional<double> OrderBook::mid() const {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return (static_cast<double>(bid->price_ticks) + static_cast<double>(ask->price_ticks)) / 2.0;
}

std::optional<Price> OrderBook::spread() const {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return ask->price_ticks - bid->price_ticks;
}

std::optional<BookLevel> OrderBook::level(const data::BookSide side,
                                          const std::size_t depth) const {
  if (side == data::BookSide::Bid) {
    if (depth >= bids_.size()) {
      return std::nullopt;
    }
    auto it = bids_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(depth));
    return to_book_level(it);
  }

  if (depth >= asks_.size()) {
    return std::nullopt;
  }
  auto it = asks_.begin();
  std::advance(it, static_cast<std::ptrdiff_t>(depth));
  return to_book_level(it);
}

bool OrderBook::empty() const {
  return bids_.empty() && asks_.empty();
}

std::size_t OrderBook::bid_depth() const {
  return bids_.size();
}

std::size_t OrderBook::ask_depth() const {
  return asks_.size();
}

bool OrderBook::is_crossed_or_locked() const {
  return crossed_or_locked(bids_, asks_);
}

std::uint64_t OrderBook::snapshot_id() const {
  return snapshot_id_;
}

void OrderBook::set_level(const data::BookSide side, const Price price_ticks,
                          const Quantity quantity_lots) {
  validate_level(price_ticks, quantity_lots);

  if (side == data::BookSide::Bid) {
    if (quantity_lots == 0) {
      bids_.erase(price_ticks);
    } else {
      bids_[price_ticks] = quantity_lots;
    }
  } else if (quantity_lots == 0) {
    asks_.erase(price_ticks);
  } else {
    asks_[price_ticks] = quantity_lots;
  }
}

void OrderBook::restore_level(const data::BookSide side, const Price price_ticks,
                              const std::optional<Quantity> quantity) {
  if (side == data::BookSide::Bid) {
    if (quantity) {
      bids_[price_ticks] = *quantity;
    } else {
      bids_.erase(price_ticks);
    }
  } else if (quantity) {
    asks_[price_ticks] = *quantity;
  } else {
    asks_.erase(price_ticks);
  }
}

void OrderBook::trim_to_max_depth() {
  trim_levels(bids_, config_.max_depth);
  trim_levels(asks_, config_.max_depth);
}

void OrderBook::enforce_crossed_policy(const std::optional<data::BookSide> changed_side) {
  if (!is_crossed_or_locked()) {
    return;
  }

  switch (config_.crossed_book_policy) {
  case CrossedBookPolicy::Reject:
    throw std::runtime_error("OrderBook is crossed or locked");
  case CrossedBookPolicy::DropCrossingLevels:
    recover_crossed_book(changed_side);
    return;
  case CrossedBookPolicy::IgnoreWithWarning:
    spdlog::warn("OrderBook crossed/locked state kept: best_bid={} best_ask={}",
                 bids_.begin()->first, asks_.begin()->first);
    return;
  }
}

void OrderBook::recover_crossed_book(const std::optional<data::BookSide> changed_side) {
  while (is_crossed_or_locked()) {
    spdlog::warn("OrderBook crossed/locked recovery: best_bid={} best_ask={}", bids_.begin()->first,
                 asks_.begin()->first);

    if (changed_side == data::BookSide::Bid) {
      bids_.erase(bids_.begin());
      continue;
    }

    if (changed_side == data::BookSide::Ask) {
      asks_.erase(asks_.begin());
      continue;
    }

    if (bids_.begin()->second <= asks_.begin()->second) {
      bids_.erase(bids_.begin());
    } else {
      asks_.erase(asks_.begin());
    }
  }
}

} // namespace lob::book
