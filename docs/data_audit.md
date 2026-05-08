# Data Audit

Audit date: 2026-05-08.

Command:

```bash
python3 lob_backtester/scripts/python/audit.py --json-out data/sample/audit_summary.json
```

The archive `data/MD.zip` contains two CSV files: `lob.csv` and `trades.csv`.
Both files are sorted by `local_timestamp` and use Unix timestamps in
microseconds.

## Source Files

| File | Format | Size | Rows | Native event type | Time range UTC | Sort check |
| --- | --- | ---: | ---: | --- | --- | --- |
| `data/raw/lob.csv` | CSV | 969,925,308 B | 1,036,690 | `book_snapshot_25` | 2024-08-01 00:00:02.038431 - 2024-08-06 23:59:59.947411 | monotonic, 0 violations |
| `data/raw/trades.csv` | CSV | 991,873,579 B | 21,864,989 | `trade` | 2024-08-01 00:00:00.014926 - 2024-08-06 23:59:59.849954 | monotonic, 0 violations |

Native `depth_update` events are not present in the archive. The book stream is
a sequence of full 25-level snapshots, not incremental level updates.

## Event Counts

| Event type | Count | Source |
| --- | ---: | --- |
| `book_snapshot_25` | 1,036,690 | `lob.csv` |
| `trade` | 21,864,989 | `trades.csv` |
| `depth_update` | 0 | Not present in raw data |

## Schema

Common fields:

| Column | Type | Range / values | Notes |
| --- | --- | --- | --- |
| `row_id` | int64 | `0..N-1` per file | The first CSV column is unnamed; audit normalizes it to `row_id`. It is a per-file row number, not a global sequence. |
| `local_timestamp` | int64 | `1722470400014926..1722988799947411` | Unix timestamp in microseconds, UTC. |

`lob.csv` columns:

| Column pattern | Type | Range / values | Notes |
| --- | --- | --- | --- |
| `asks[i].price` | decimal | observed `0.0058449..0.0111469` | Ask price at depth `i`, `i = 0..24`. |
| `asks[i].amount` | decimal | observed `1.0..168666220.0` | Ask size at depth `i`. Values are whole units represented as decimals. |
| `bids[i].price` | decimal | observed `0.0058449..0.0111469` | Bid price at depth `i`, `i = 0..24`. |
| `bids[i].amount` | decimal | observed `1.0..168666220.0` | Bid size at depth `i`. Values are whole units represented as decimals. |

`trades.csv` columns:

| Column | Type | Range / values | Notes |
| --- | --- | --- | --- |
| `side` | string | `buy`, `sell` | Trade side from the data source. Treat as aggressor side only after confirming the venue convention; fill model can use `price` without side. |
| `price` | decimal | `0.0058452..0.0111441` | Trade price. |
| `amount` | int64 | `1..179090276` | Trade amount. |

## Example Records

`book_snapshot_25` example from `lob.csv`, row `0`:

```json
{
  "event_type": "book_snapshot_25",
  "row_id": 0,
  "local_timestamp": 1722470402038431,
  "asks": [
    {"level": 0, "price": 0.0110436, "amount": 121492.0},
    {"level": 1, "price": 0.0110438, "amount": 4663.0},
    {"level": 24, "price": 0.0110469, "amount": 1000463.0}
  ],
  "bids": [
    {"level": 0, "price": 0.0110435, "amount": 103687.0},
    {"level": 1, "price": 0.0110433, "amount": 36226.0},
    {"level": 24, "price": 0.0110402, "amount": 90028.0}
  ]
}
```

The raw row contains all 25 ask levels and all 25 bid levels; the example above
keeps the top two levels and the deepest level for readability.

`trade` example from `trades.csv`, row `0`:

```json
{
  "event_type": "trade",
  "row_id": 0,
  "local_timestamp": 1722470400014926,
  "side": "sell",
  "price": 0.0110435,
  "amount": 734
}
```

`depth_update` example: not applicable. No native incremental update records are
available in `MD.zip`.

## Fixed Assumptions For The Loader

| Field | Decision |
| --- | --- |
| Symbol | Raw files do not include an instrument symbol. Use internal dataset symbol `MD` until the original venue/instrument is confirmed. |
| `tick_size` | `0.0000001` (`1e-7`), matching observed best bid/ask one-tick spreads and 7 decimal-place prices. |
| `lot_size` | `1.0`, because sizes are whole units even when LOB amounts are represented with `.0`. |
| Timestamp unit | Microseconds since Unix epoch, UTC. |
| Sequence | No global sequence column. Use `(local_timestamp, source_priority, row_id)` for deterministic merge. |
| Book depth | 25 levels per side. |

Initial merge priority for equal timestamps: `book_snapshot_25` before `trade`,
then `row_id`. This keeps the book state current before evaluating a trade at
the same timestamp. If later validation finds this convention wrong, only the
data merge comparator should change.

## Sample Dataset

The sample is the busiest one-hour interval by combined event count:

`2024-08-05 06:00:00 UTC <= local_timestamp < 2024-08-05 07:00:00 UTC`.

| File | Rows | Size | Covers |
| --- | ---: | ---: | --- |
| `data/sample/lob.csv` | 7,200 | 6,717,479 B | `book_snapshot_25` |
| `data/sample/trades.csv` | 750,467 | 34,578,559 B | `trade` |
| `data/sample/manifest.json` | - | 305 B | sample metadata |
| `data/sample/audit_summary.json` | - | ~38 KB | machine-readable audit output |

Both CSV sample files are below 50 MB. The sample covers all native event types
available in the raw archive. It cannot cover native `depth_update` events
because none exist in the source data.

## C++ Loader Throughput

Measured on 2026-05-08 with the integration test:

```bash
./build/lob_tests --gtest_filter=CsvDataSourceIntegrationTest.DrainsSampleWithAuditCounts
```

Result on `data/sample`:

| Metric | Value |
| --- | ---: |
| Total events | 757,667 |
| `book_snapshot_25` events | 7,200 |
| `trade` events | 750,467 |
| `depth_update` events | 0 |
| Throughput | 3,341,723 events/sec |

## Preprocessing Decision

MVP decision: parse raw CSV directly in the C++ engine.

Rationale:

- The archive is CSV-only; no parquet/jsonl support is needed for this dataset.
- Files are already sorted by timestamp within each stream.
- The engine can stream `lob.csv` and `trades.csv` and merge them by
  `(local_timestamp, source_priority, row_id)` without loading the full dataset
  into memory.
- Introducing Arrow IPC or a packed binary format now would add build and
  operational complexity before we have replay throughput measurements.

Future option: add a preprocessing stage from CSV to a packed binary event log if
T3 replay benchmarking shows CSV parsing is a bottleneck.
