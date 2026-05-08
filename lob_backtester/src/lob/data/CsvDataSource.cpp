#include "lob/data/CsvDataSource.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lob::data {
namespace {

enum class SourceKind : std::uint8_t {
  Snapshot = 0,
  DepthUpdate = 1,
  Trade = 2,
};

constexpr std::uint64_t kSequencePriorityShift = 56;
constexpr std::uint64_t kMaxRowIdInSequence =
    (std::uint64_t{1} << kSequencePriorityShift) - 1;

std::runtime_error parse_error(const std::filesystem::path &path,
                               const std::uint64_t line_number,
                               const std::string_view message) {
  return std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                            ": " + std::string(message));
}

std::vector<std::string_view> split_csv_line(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }

  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = line.find(',', start);
    if (end == std::string_view::npos) {
      fields.emplace_back(line.substr(start));
      break;
    }
    fields.emplace_back(line.substr(start, end - start));
    start = end + 1;
  }
  return fields;
}

std::int64_t parse_i64(std::string_view text,
                       const std::filesystem::path &path,
                       const std::uint64_t line_number,
                       const std::string_view field_name) {
  std::int64_t value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw parse_error(path, line_number,
                      "invalid int64 field '" + std::string(field_name) + "'");
  }
  return value;
}

std::uint64_t parse_u64(std::string_view text,
                        const std::filesystem::path &path,
                        const std::uint64_t line_number,
                        const std::string_view field_name) {
  std::uint64_t value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw parse_error(path, line_number,
                      "invalid uint64 field '" + std::string(field_name) + "'");
  }
  return value;
}

double parse_double(std::string_view text,
                    const std::filesystem::path &path,
                    const std::uint64_t line_number,
                    const std::string_view field_name) {
  double value = 0.0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end ||
      !std::isfinite(value)) {
    throw parse_error(path, line_number,
                      "invalid decimal field '" + std::string(field_name) + "'");
  }
  return value;
}

std::int64_t normalize_to_units(const double value,
                                const double unit,
                                const std::filesystem::path &path,
                                const std::uint64_t line_number,
                                const std::string_view field_name) {
  if (unit <= 0.0 || !std::isfinite(unit)) {
    throw std::runtime_error("normalization unit must be positive for field '" +
                             std::string(field_name) + "'");
  }

  const double scaled = value / unit;
  const double rounded = std::round(scaled);
  const double tolerance = std::max(1.0e-6, std::abs(rounded) * 1.0e-12);
  if (std::abs(scaled - rounded) > tolerance) {
    throw parse_error(path, line_number,
                      "field '" + std::string(field_name) +
                          "' is not aligned to configured unit");
  }
  if (rounded < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      rounded > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    throw parse_error(path, line_number,
                      "field '" + std::string(field_name) +
                          "' overflows int64 units");
  }

  return static_cast<std::int64_t>(rounded);
}

std::int64_t parse_normalized(std::string_view text,
                              const double unit,
                              const std::filesystem::path &path,
                              const std::uint64_t line_number,
                              const std::string_view field_name) {
  return normalize_to_units(
      parse_double(text, path, line_number, field_name), unit, path, line_number,
      field_name);
}

TradeSide parse_trade_side(std::string_view text,
                           const std::filesystem::path &path,
                           const std::uint64_t line_number) {
  if (text == "buy") {
    return TradeSide::Buy;
  }
  if (text == "sell") {
    return TradeSide::Sell;
  }
  throw parse_error(path, line_number, "invalid trade side");
}

BookSide parse_book_side(std::string_view text,
                         const std::filesystem::path &path,
                         const std::uint64_t line_number) {
  if (text == "ask") {
    return BookSide::Ask;
  }
  if (text == "bid") {
    return BookSide::Bid;
  }
  throw parse_error(path, line_number, "invalid book side");
}

std::uint64_t make_sequence(const SourceKind kind,
                            const std::uint64_t row_id,
                            const std::filesystem::path &path,
                            const std::uint64_t line_number) {
  if (row_id > kMaxRowIdInSequence) {
    throw parse_error(path, line_number,
                      "row_id is too large to encode into sequence");
  }

  const auto priority = static_cast<std::uint64_t>(kind);
  return (priority << kSequencePriorityShift) | row_id;
}

std::int64_t timestamp_us_to_ns(const std::int64_t timestamp_us,
                                const std::filesystem::path &path,
                                const std::uint64_t line_number) {
  constexpr std::int64_t kNsPerUs = 1000;
  if (timestamp_us >
          std::numeric_limits<std::int64_t>::max() / kNsPerUs ||
      timestamp_us <
          std::numeric_limits<std::int64_t>::min() / kNsPerUs) {
    throw parse_error(path, line_number, "timestamp overflows nanoseconds");
  }
  return timestamp_us * kNsPerUs;
}

MarketEvent parse_snapshot_row(const std::vector<std::string_view> &fields,
                               const CsvDataSourceConfig &config,
                               const std::filesystem::path &path,
                               const std::uint64_t line_number) {
  const std::size_t expected_min_fields = 2 + config.snapshot_depth * 4;
  if (fields.size() < expected_min_fields) {
    throw parse_error(path, line_number, "snapshot row has too few columns");
  }

  const std::uint64_t row_id = parse_u64(fields[0], path, line_number, "row_id");
  const std::int64_t timestamp_us =
      parse_i64(fields[1], path, line_number, "local_timestamp");

  MarketEvent event{};
  event.ts_ns = timestamp_us_to_ns(timestamp_us, path, line_number);
  event.seq = make_sequence(SourceKind::Snapshot, row_id, path, line_number);
  event.type = EventType::Snapshot;
  event.payload.snapshot.depth =
      static_cast<std::uint8_t>(config.snapshot_depth);

  for (std::size_t level = 0; level < config.snapshot_depth; ++level) {
    const std::size_t base = 2 + level * 4;
    event.payload.snapshot.asks[level] = PriceLevel{
        parse_normalized(fields[base], config.tick_size, path, line_number,
                         "ask_price"),
        parse_normalized(fields[base + 1], config.lot_size, path, line_number,
                         "ask_amount")};
    event.payload.snapshot.bids[level] = PriceLevel{
        parse_normalized(fields[base + 2], config.tick_size, path, line_number,
                         "bid_price"),
        parse_normalized(fields[base + 3], config.lot_size, path, line_number,
                         "bid_amount")};
  }

  return event;
}

MarketEvent parse_depth_update_row(const std::vector<std::string_view> &fields,
                                   const CsvDataSourceConfig &config,
                                   const std::filesystem::path &path,
                                   const std::uint64_t line_number) {
  if (fields.size() < 5) {
    throw parse_error(path, line_number, "depth update row has too few columns");
  }

  const std::uint64_t row_id = parse_u64(fields[0], path, line_number, "row_id");
  const std::int64_t timestamp_us =
      parse_i64(fields[1], path, line_number, "local_timestamp");

  MarketEvent event{};
  event.ts_ns = timestamp_us_to_ns(timestamp_us, path, line_number);
  event.seq = make_sequence(SourceKind::DepthUpdate, row_id, path, line_number);
  event.type = EventType::DepthUpdate;
  event.payload.depth_update.side =
      parse_book_side(fields[2], path, line_number);
  event.payload.depth_update.price_ticks =
      parse_normalized(fields[3], config.tick_size, path, line_number, "price");
  event.payload.depth_update.quantity_lots = parse_normalized(
      fields[4], config.lot_size, path, line_number, "amount");
  return event;
}

MarketEvent parse_trade_row(const std::vector<std::string_view> &fields,
                            const CsvDataSourceConfig &config,
                            const std::filesystem::path &path,
                            const std::uint64_t line_number) {
  if (fields.size() < 5) {
    throw parse_error(path, line_number, "trade row has too few columns");
  }

  const std::uint64_t row_id = parse_u64(fields[0], path, line_number, "row_id");
  const std::int64_t timestamp_us =
      parse_i64(fields[1], path, line_number, "local_timestamp");

  MarketEvent event{};
  event.ts_ns = timestamp_us_to_ns(timestamp_us, path, line_number);
  event.seq = make_sequence(SourceKind::Trade, row_id, path, line_number);
  event.type = EventType::Trade;
  event.payload.trade.side = parse_trade_side(fields[2], path, line_number);
  event.payload.trade.price_ticks =
      parse_normalized(fields[3], config.tick_size, path, line_number, "price");
  event.payload.trade.quantity_lots = parse_normalized(
      fields[4], config.lot_size, path, line_number, "amount");
  return event;
}

MarketEvent parse_row(const std::vector<std::string_view> &fields,
                      const SourceKind kind,
                      const CsvDataSourceConfig &config,
                      const std::filesystem::path &path,
                      const std::uint64_t line_number) {
  switch (kind) {
  case SourceKind::Snapshot:
    return parse_snapshot_row(fields, config, path, line_number);
  case SourceKind::DepthUpdate:
    return parse_depth_update_row(fields, config, path, line_number);
  case SourceKind::Trade:
    return parse_trade_row(fields, config, path, line_number);
  }
  throw parse_error(path, line_number, "unknown source kind");
}

struct BufferedSource {
  BufferedSource(SourceKind source_kind,
                 std::filesystem::path source_path,
                 CsvDataSourceConfig source_config)
      : kind(source_kind), path(std::move(source_path)),
        config(std::move(source_config)), stream(path) {
    if (!stream) {
      throw std::runtime_error("failed to open CSV source: " + path.string());
    }

    std::string header;
    if (!std::getline(stream, header)) {
      throw std::runtime_error("CSV source is empty: " + path.string());
    }
    line_number = 1;
    advance();
  }

  bool advance() {
    while (std::getline(stream, line)) {
      ++line_number;
      if (line.empty() || line == "\r") {
        continue;
      }

      const auto fields = split_csv_line(line);
      current = parse_row(fields, kind, config, path, line_number);
      if (last_ts_ns.has_value() && current.ts_ns < *last_ts_ns) {
        throw parse_error(path, line_number,
                          "timestamp regression inside CSV source");
      }
      last_ts_ns = current.ts_ns;
      has_current = true;
      current_consumed = false;
      return true;
    }

    has_current = false;
    return false;
  }

  SourceKind kind;
  std::filesystem::path path;
  CsvDataSourceConfig config;
  std::ifstream stream;
  std::string line;
  std::uint64_t line_number = 0;
  std::optional<std::int64_t> last_ts_ns;
  MarketEvent current{};
  bool has_current = false;
  bool current_consumed = false;
};

bool event_less(const MarketEvent &left, const MarketEvent &right) {
  if (left.ts_ns != right.ts_ns) {
    return left.ts_ns < right.ts_ns;
  }
  return left.seq < right.seq;
}

} // namespace

CsvDataSourceConfig csv_config_from_directory(const std::filesystem::path &input_dir,
                                              const double tick_size,
                                              const double lot_size) {
  CsvDataSourceConfig config;
  config.snapshots_path = input_dir / "lob.csv";
  config.trades_path = input_dir / "trades.csv";
  const std::filesystem::path depth_updates_path = input_dir / "depth_updates.csv";
  if (std::filesystem::exists(depth_updates_path)) {
    config.depth_updates_path = depth_updates_path;
  }
  config.tick_size = tick_size;
  config.lot_size = lot_size;
  return config;
}

class CsvDataSource::Impl {
public:
  explicit Impl(CsvDataSourceConfig config) {
    if (config.snapshot_depth == 0 || config.snapshot_depth > kSnapshotDepth) {
      throw std::runtime_error("snapshot_depth must be in range 1..25");
    }

    add_source(SourceKind::Snapshot, config.snapshots_path, config);
    add_source(SourceKind::DepthUpdate, config.depth_updates_path, config);
    add_source(SourceKind::Trade, config.trades_path, config);

    if (sources_.empty()) {
      throw std::runtime_error("CsvDataSource requires at least one CSV path");
    }
  }

  bool next(MarketEvent &event) {
    refresh_consumed_sources();
    if (sources_.empty()) {
      return false;
    }

    const auto best = std::min_element(
        sources_.begin(), sources_.end(),
        [](const auto &left, const auto &right) {
          return event_less(left->current, right->current);
        });

    const MarketEvent candidate = (*best)->current;
    validate_global_order(candidate);
    event = candidate;
    (*best)->current_consumed = true;
    return true;
  }

private:
  void refresh_consumed_sources() {
    auto source = sources_.begin();
    while (source != sources_.end()) {
      if (!(*source)->current_consumed) {
        ++source;
        continue;
      }
      if ((*source)->advance()) {
        ++source;
        continue;
      }
      source = sources_.erase(source);
    }
  }

  void add_source(const SourceKind kind,
                  const std::filesystem::path &path,
                  const CsvDataSourceConfig &config) {
    if (path.empty()) {
      return;
    }
    sources_.push_back(
        std::make_unique<BufferedSource>(kind, path, config));
    if (!sources_.back()->has_current) {
      sources_.pop_back();
    }
  }

  void validate_global_order(const MarketEvent &event) {
    if (!last_event_.has_value()) {
      last_event_ = event;
      return;
    }

    if (event.ts_ns < last_event_->ts_ns) {
      throw std::runtime_error("global event timestamp regression");
    }
    if (event.ts_ns == last_event_->ts_ns && event.seq == last_event_->seq) {
      throw std::runtime_error("duplicate global event key");
    }

    last_event_ = event;
  }

  std::vector<std::unique_ptr<BufferedSource>> sources_;
  std::optional<MarketEvent> last_event_;
};

CsvDataSource::CsvDataSource(CsvDataSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

CsvDataSource::~CsvDataSource() = default;

CsvDataSource::CsvDataSource(CsvDataSource &&) noexcept = default;

CsvDataSource &CsvDataSource::operator=(CsvDataSource &&) noexcept = default;

bool CsvDataSource::next(MarketEvent &event) {
  return impl_->next(event);
}

} // namespace lob::data
