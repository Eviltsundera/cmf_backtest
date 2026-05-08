#pragma once

#include "lob/data/MarketEvent.hpp"

#include <cstdint>

namespace lob::data {

class IDataSource {
public:
  virtual ~IDataSource() = default;

  virtual bool next(MarketEvent &event) = 0;
};

struct EventCounts {
  std::uint64_t snapshots = 0;
  std::uint64_t depth_updates = 0;
  std::uint64_t trades = 0;

  [[nodiscard]] std::uint64_t total() const {
    return snapshots + depth_updates + trades;
  }
};

inline void count_event(const MarketEvent &event, EventCounts &counts) {
  switch (event.type) {
  case EventType::Snapshot:
    ++counts.snapshots;
    break;
  case EventType::DepthUpdate:
    ++counts.depth_updates;
    break;
  case EventType::Trade:
    ++counts.trades;
    break;
  }
}

inline EventCounts drain_data_source(IDataSource &source) {
  EventCounts counts;
  MarketEvent event{};
  while (source.next(event)) {
    count_event(event, counts);
  }
  return counts;
}

} // namespace lob::data
