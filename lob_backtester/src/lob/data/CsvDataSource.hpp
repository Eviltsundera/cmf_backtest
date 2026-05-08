#pragma once

#include "lob/data/DataSource.hpp"

#include <filesystem>
#include <memory>

namespace lob::data {

struct CsvDataSourceConfig {
  std::filesystem::path snapshots_path;
  std::filesystem::path depth_updates_path;
  std::filesystem::path trades_path;
  double tick_size = 0.0000001;
  double lot_size = 1.0;
  std::size_t snapshot_depth = kSnapshotDepth;
};

CsvDataSourceConfig csv_config_from_directory(const std::filesystem::path &input_dir,
                                              double tick_size,
                                              double lot_size);

class CsvDataSource final : public IDataSource {
public:
  explicit CsvDataSource(CsvDataSourceConfig config);
  ~CsvDataSource() override;

  CsvDataSource(const CsvDataSource &) = delete;
  CsvDataSource &operator=(const CsvDataSource &) = delete;
  CsvDataSource(CsvDataSource &&) noexcept;
  CsvDataSource &operator=(CsvDataSource &&) noexcept;

  bool next(MarketEvent &event) override;

private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

} // namespace lob::data
