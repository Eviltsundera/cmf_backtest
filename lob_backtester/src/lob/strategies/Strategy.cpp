#include "lob/strategies/Strategy.hpp"

namespace lob::strategies {

std::vector<execution::OrderIntent> NoopStrategy::on_market_event(const data::MarketEvent &,
                                                                  const MarketState &) {
  return {};
}

std::vector<execution::OrderIntent> NoopStrategy::on_fill(const execution::Fill &,
                                                          const MarketState &) {
  return {};
}

} // namespace lob::strategies
