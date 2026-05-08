#pragma once

#include <cstddef>
#include <cstdint>

namespace lob::data {

inline constexpr std::size_t kSnapshotDepth = 25;

enum class EventType : std::uint8_t {
  Snapshot = 0,
  DepthUpdate = 1,
  Trade = 2,
};

enum class BookSide : std::uint8_t {
  Ask = 0,
  Bid = 1,
};

enum class TradeSide : std::uint8_t {
  Buy = 0,
  Sell = 1,
};

struct PriceLevel {
  std::int64_t price_ticks;
  std::int64_t quantity_lots;
};

struct SnapshotPayload {
  std::uint8_t depth;
  PriceLevel asks[kSnapshotDepth];
  PriceLevel bids[kSnapshotDepth];
};

struct DepthUpdatePayload {
  BookSide side;
  std::int64_t price_ticks;
  std::int64_t quantity_lots;
};

struct TradePayload {
  TradeSide side;
  std::int64_t price_ticks;
  std::int64_t quantity_lots;
};

union MarketEventPayload {
  SnapshotPayload snapshot;
  DepthUpdatePayload depth_update;
  TradePayload trade;
};

struct MarketEvent {
  std::int64_t ts_ns;
  std::uint64_t seq;
  EventType type;
  MarketEventPayload payload;
};

} // namespace lob::data
