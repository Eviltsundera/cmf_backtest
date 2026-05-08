#include "lob/engine/BacktestEngine.hpp"

#include "lob/features/FeatureEngine.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace lob::engine {
namespace {

struct ActiveQuotes {
  std::optional<double> bid;
  std::optional<double> ask;
};

struct PendingAdverseSelection {
  execution::Fill fill;
  std::int64_t horizon_ns = 0;
  std::int64_t target_ts_ns = 0;
};

struct PendingAdverseSelectionDueFirst {
  bool operator()(const PendingAdverseSelection &lhs, const PendingAdverseSelection &rhs) const {
    return lhs.target_ts_ns > rhs.target_ts_ns;
  }
};

using PendingAdverseSelectionQueue =
    std::priority_queue<PendingAdverseSelection, std::vector<PendingAdverseSelection>,
                        PendingAdverseSelectionDueFirst>;

void validate_config(const BacktestEngineConfig &config) {
  if (!std::isfinite(config.initial_cash)) {
    throw std::runtime_error("BacktestEngine initial_cash must be finite");
  }
  if (config.quote_refresh_ns < 0) {
    throw std::runtime_error("BacktestEngine quote_refresh_ns must be non-negative");
  }
  for (const std::int64_t horizon_ns : config.adverse_selection_horizons_ns) {
    if (horizon_ns <= 0) {
      throw std::runtime_error("BacktestEngine adverse-selection horizons must be positive");
    }
  }
}

bool should_call_strategy(const std::optional<std::int64_t> last_call_ts_ns,
                          const std::int64_t event_ts_ns, const std::int64_t quote_refresh_ns) {
  if (!last_call_ts_ns.has_value()) {
    return true;
  }
  if (quote_refresh_ns == 0) {
    return true;
  }
  return event_ts_ns - *last_call_ts_ns >= quote_refresh_ns;
}

ActiveQuotes active_quotes(const execution::OrderManager &orders) {
  ActiveQuotes quotes;
  for (const execution::OrderId order_id : orders.active_order_ids()) {
    const execution::Order *order = orders.find_order(order_id);
    if (order == nullptr || order->remaining_lots <= 0) {
      continue;
    }

    const double price = static_cast<double>(order->price_ticks);
    if (order->side == execution::OrderSide::Buy) {
      if (!quotes.bid || price > *quotes.bid) {
        quotes.bid = price;
      }
    } else if (!quotes.ask || price < *quotes.ask) {
      quotes.ask = price;
    }
  }

  if (quotes.bid && quotes.ask && *quotes.bid >= *quotes.ask) {
    return {};
  }
  return quotes;
}

void process_intents(const std::vector<execution::OrderIntent> &intents,
                     execution::OrderManager &orders, const book::OrderBook &book,
                     const portfolio::Portfolio &portfolio, metrics::MetricsEngine &metrics) {
  for (const execution::OrderIntent &intent : intents) {
    const execution::OrderIntentResult result =
        orders.process_intent(intent, &book, portfolio.position_lots());
    const bool submits_new_order = intent.type == execution::OrderIntentType::SubmitLimit ||
                                   intent.type == execution::OrderIntentType::Replace;
    if (submits_new_order && result.accepted) {
      metrics.record_fill_opportunity();
    }
  }
}

std::int64_t target_timestamp_ns(const execution::Fill &fill, const std::int64_t horizon_ns) {
  if (fill.ts_ns > std::numeric_limits<std::int64_t>::max() - horizon_ns) {
    throw std::runtime_error("BacktestEngine adverse-selection target timestamp overflows");
  }
  return fill.ts_ns + horizon_ns;
}

void enqueue_adverse_selection(const execution::Fill &fill,
                               const std::vector<std::int64_t> &horizons_ns,
                               PendingAdverseSelectionQueue &pending) {
  for (const std::int64_t horizon_ns : horizons_ns) {
    pending.push(PendingAdverseSelection{
        .fill = fill,
        .horizon_ns = horizon_ns,
        .target_ts_ns = target_timestamp_ns(fill, horizon_ns),
    });
  }
}

void record_due_adverse_selection(const std::int64_t event_ts_ns, const book::OrderBook &book,
                                  metrics::MetricsEngine &metrics,
                                  PendingAdverseSelectionQueue &pending) {
  if (pending.empty() || pending.top().target_ts_ns > event_ts_ns) {
    return;
  }

  const std::optional<double> future_mid_price = book.mid();
  if (!future_mid_price) {
    return;
  }

  while (!pending.empty() && pending.top().target_ts_ns <= event_ts_ns) {
    const PendingAdverseSelection entry = pending.top();
    metrics.record_adverse_selection(entry.fill, entry.horizon_ns, *future_mid_price);
    pending.pop();
  }
}

void write_outputs(const std::filesystem::path &output_dir, const metrics::MetricsEngine &metrics,
                   const execution::OrderManager &orders,
                   const std::vector<execution::Fill> &fills) {
  if (output_dir.empty()) {
    return;
  }

  std::filesystem::create_directories(output_dir);
  metrics.write_run_outputs(output_dir);
  orders.write_order_log_csv(output_dir / "orders.csv");
  write_fills_csv(output_dir / "fills.csv", fills);
}

} // namespace

BacktestEngine::BacktestEngine(BacktestEngineConfig config) : config_(std::move(config)) {
  validate_config(config_);
}

BacktestResult BacktestEngine::run(data::IDataSource &source,
                                   strategies::IStrategy &strategy) const {
  book::OrderBook book(config_.book);
  execution::OrderManager orders(config_.orders);
  execution::FillModel fill_model(config_.fills);
  portfolio::Portfolio portfolio(config_.initial_cash);
  metrics::MetricsEngine metrics;
  std::vector<execution::Fill> all_fills;
  PendingAdverseSelectionQueue pending_adverse_selection;
  std::optional<std::int64_t> last_strategy_call_ts_ns;

  const auto started_at = std::chrono::steady_clock::now();
  data::EventCounts counts;
  data::MarketEvent event{};
  while (source.next(event)) {
    data::count_event(event, counts);
    book.apply_event(event);
    record_due_adverse_selection(event.ts_ns, book, metrics, pending_adverse_selection);

    const std::vector<execution::Fill> fills = fill_model.fill_active_orders(orders, event, book);
    for (const execution::Fill &fill : fills) {
      portfolio.apply_fill(fill);
      metrics.record_fill(fill, book.mid());
      enqueue_adverse_selection(fill, config_.adverse_selection_horizons_ns,
                                pending_adverse_selection);
      all_fills.push_back(fill);
    }

    for (const execution::Fill &fill : fills) {
      const strategies::MarketState fill_state = make_market_state(event, book, orders, portfolio);
      process_intents(strategy.on_fill(fill, fill_state), orders, book, portfolio, metrics);
    }

    if (should_call_strategy(last_strategy_call_ts_ns, event.ts_ns, config_.quote_refresh_ns)) {
      const strategies::MarketState state = make_market_state(event, book, orders, portfolio);
      process_intents(strategy.on_market_event(event, state), orders, book, portfolio, metrics);
      last_strategy_call_ts_ns = event.ts_ns;
    }

    const ActiveQuotes quotes = active_quotes(orders);
    metrics.record_quote(event.ts_ns, quotes.bid, quotes.ask);
    if (const auto mark_price = book.mid()) {
      metrics.record_equity(event.ts_ns, portfolio, *mark_price);
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
  write_outputs(config_.output_dir, metrics, orders, all_fills);

  BacktestResult result;
  result.event_counts = counts;
  result.fill_count = all_fills.size();
  result.order_event_count = orders.event_log().size();
  result.active_order_count = orders.active_count();
  result.elapsed_seconds = elapsed_seconds;
  if (elapsed_seconds > 0.0) {
    result.events_per_second = static_cast<double>(counts.total()) / elapsed_seconds;
  }
  result.portfolio = portfolio;
  result.metrics = metrics.compute();
  result.fills = std::move(all_fills);
  return result;
}

strategies::MarketState make_market_state(const data::MarketEvent &event,
                                          const book::OrderBook &book,
                                          const execution::OrderManager &orders,
                                          const portfolio::Portfolio &portfolio) {
  strategies::MarketState state;
  state.ts_ns = event.ts_ns;
  state.event_seq = event.seq;
  state.best_bid = book.best_bid();
  state.best_ask = book.best_ask();
  state.mid_price = features::mid(book);
  state.spread_ticks = book.spread();
  state.imbalance = features::imbalance(book);
  state.weighted_mid = features::weighted_mid(book);
  state.inventory_lots = portfolio.position_lots();
  state.cash = portfolio.cash();
  state.active_order_count = orders.active_count();
  return state;
}

void write_fills_csv(const std::filesystem::path &path, const std::vector<execution::Fill> &fills) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Unable to open fills log: " + path.string());
  }

  out << "ts_ns,order_id,strategy_id,side,limit_price_ticks,fill_price_ticks,"
         "quantity_lots,liquidity_role,fee\n";
  for (const execution::Fill &fill : fills) {
    out << fill.ts_ns << ',' << fill.order_id << ',' << fill.strategy_id << ','
        << execution::to_string(fill.side) << ',' << fill.limit_price_ticks << ','
        << fill.fill_price_ticks << ',' << fill.quantity_lots << ','
        << execution::to_string(fill.liquidity_role) << ',' << fill.fee << '\n';
  }
}

} // namespace lob::engine
