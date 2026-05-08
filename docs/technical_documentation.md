# Техническая документация: LOB Backtester

**Статус:** draft, синхронизирован с T11 Microprice extension.

Документ описывает целевую архитектуру CMF LOB backtesting engine и то, что уже
есть в репозитории. Детальный task tracker остается в
`docs/implementation_plan.md`.

## 1. Назначение

Проект реализует event-driven backtest engine для исторических данных
лимитной книги. Движок должен воспроизводить market data, поддерживать
агрегированную LOB, моделировать жизненный цикл собственных лимитных ордеров,
симулировать исполнения, считать торговые метрики и сравнивать market-making
стратегии:

- fixed-spread baseline;
- Avellaneda-Stoikov 2008;
- microprice-adjusted Avellaneda-Stoikov.

MVP использует консервативную модель исполнения: ордер исполняется, когда
рыночная цена пересекает его limit level. Queue priority, market impact,
latency и partial fills считаются расширениями, если отдельная будущая задача
не включает их явно.

## 2. Текущая реализация

Сейчас кодовая база находится на уровне T11 Microprice extension:

- `lob_backtester/CMakeLists.txt` определяет `lob_core`, `lob_backtest` и
  `lob_tests`.
- `lob_backtester/src/lob/utils/Config.hpp` и `.cpp` загружают YAML-конфиг в
  типизированные структуры.
- `lob_backtester/src/lob/data/MarketEvent.hpp` определяет нормализованный
  market-event contract.
- `lob_backtester/src/lob/data/DataSource.hpp` определяет `IDataSource` и
  счетчики событий для replay/integration checks.
- `lob_backtester/src/lob/data/CsvDataSource.hpp` и `.cpp` реализуют streaming
  CSV loader с merge по `(ts_ns, seq)`.
- `lob_backtester/src/lob/book/OrderBook.hpp` и `.cpp` реализуют
  Market-By-Price книгу с snapshots, depth updates и recovery для crossed book.
- `lob_backtester/src/lob/features/FeatureEngine.hpp` и `.cpp` считают
  top-of-book features и rolling volatility.
- `lob_backtester/src/lob/execution/OrderManager.hpp` и `.cpp` реализуют
  lifecycle собственных limit orders, risk gates и `orders.csv`.
- `lob_backtester/src/lob/execution/FillModel.hpp` и `.cpp` реализуют
  directional fill checks, maker/taker fees и active-order fill pass.
- `lob_backtester/src/lob/portfolio/Portfolio.hpp` и `.cpp` ведут cash,
  signed inventory, realized/unrealized PnL и mark-to-market equity.
- `lob_backtester/src/lob/metrics/MetricsEngine.hpp` и `.cpp` агрегируют
  run metrics и пишут `metrics.json`, `equity_curve.csv`, `inventory.csv`.
- `lob_backtester/src/lob/strategies/Strategy.hpp` и `.cpp` определяют
  `IStrategy`, `MarketState`, `NoopStrategy`, `FixedSpreadStrategy`,
  `AvellanedaStoikovStrategy`, classic/microprice fair-price modes и
  тестируемые A-S formula helpers.
- `lob_backtester/src/lob/engine/BacktestEngine.hpp` и `.cpp` реализуют
  event loop, scheduler, strategy callbacks и `fills.csv`.
- `lob_backtester/apps/lob_backtest.cpp` парсит `--config`, загружает YAML,
  запускает CSV replay с `strategy.name: noop`, `fixed_spread` или
  `avellaneda_stoikov`/`microprice_as`, печатает summary и пишет artifacts в
  `run.output_dir`.
- `lob_backtester/configs/example.yaml` содержит smoke config, согласованный с
  `docs/data_audit.md`.
- `lob_backtester/configs/baseline_fixed_spread.yaml` содержит sample run config
  для baseline fixed-spread стратегии.
- `lob_backtester/configs/avellaneda_stoikov.yaml` содержит sample run config
  для классической A-S стратегии.
- `lob_backtester/configs/microprice_as.yaml` содержит sample run config для
  microprice-adjusted A-S стратегии.
- `lob_backtester/tests/*_test.cpp` покрывают config, DataLoader, order book,
  features, OMS, fill model, portfolio accounting, metrics aggregation и
  engine integration.

## 3. Архитектура верхнего уровня

Движок строится как event-driven pipeline:

```text
Raw data
  -> DataLoader
  -> MarketEvent stream
  -> LOBBuilder / OrderBook
  -> FeatureEngine
  -> FillModel
  -> Strategy
  -> OrderManager
  -> Portfolio
  -> MetricsEngine
  -> reports and CSV/JSON artifacts
```

Порядок event loop должен оставаться фиксированным:

1. Прочитать следующий `MarketEvent`.
2. Применить событие к публичной order book.
3. Проверить активные собственные ордера на fills относительно текущего
   market state.
4. Уведомить portfolio и strategy о fills.
5. Дать strategy увидеть только текущее состояние и вернуть `OrderIntent`.
6. Дать order manager провалидировать, выставить, отменить или заменить
   ордера.
7. Обновить metrics и опциональные artifacts.

Такой порядок снижает риск look-ahead bias: стратегия не получает будущие
market data.

## 4. Ответственность модулей

### DataLoader

Читает raw historical files и отдает нормализованные события, отсортированные
по `(timestamp, sequence)`.

Целевая ответственность:

- использовать схему из `docs/data_audit.md`: CSV-only `book_snapshot_25` +
  `trade`, timestamp в microseconds;
- нормализовать timestamps в наносекунды;
- нормализовать prices и quantities к `tick_size` и `lot_size`;
- валидировать монотонность timestamp и дубликаты;
- стримить события без загрузки всего датасета в память.

Текущая реализация:

- `CsvDataSource` читает `lob.csv`, `trades.csv` и опциональный
  `depth_updates.csv`;
- `MarketEvent::ts_ns` хранит timestamp в ns;
- `MarketEvent::seq` кодирует `(source_priority, row_id)`;
- payload хранит prices как integer ticks и quantities как integer lots;
- equal-timestamp priority: snapshot, depth update, trade.

### OrderBook

Поддерживает агрегированную Market-By-Price book.

Целевая ответственность:

- применять snapshots и depth updates;
- хранить bids по убыванию цены, asks по возрастанию;
- удалять price level при нулевом size;
- отдавать best bid/ask, mid, spread и depth levels;
- детектить locked/crossed book и применять настроенную recovery policy.

### FeatureEngine

Считает переиспользуемые признаки order book:

- `mid`;
- `spread`;
- top-of-book imbalance;
- weighted mid;
- microprice proxy;
- rolling volatility estimator.

Эти признаки должны быть отделены от конкретных strategies.

### Strategy Interface

Определяет callbacks для event-driven стратегий:

- `on_market_event(const MarketEvent&, const MarketState&)`;
- `on_fill(const Fill&, const MarketState&)`.

`MarketState` содержит только уже примененное состояние текущего события:
best bid/ask, mid, spread, imbalance, weighted mid, microprice proxy,
inventory, cash и active-order count.

### BacktestEngine

Склеивает реализованные модули в фиксированном порядке:

1. читает `MarketEvent`;
2. применяет событие к `OrderBook`;
3. проверяет active orders через `FillModel`;
4. применяет fills к `Portfolio` и `MetricsEngine`;
5. уведомляет strategy через `on_fill`;
6. вызывает `on_market_event` по `quote_refresh_ms`;
7. отправляет `OrderIntent` в `OrderManager`;
8. записывает quote/equity samples и artifacts.

Strategy не получает будущие events: все callbacks строятся из текущего
события после `OrderBook::apply_event`.

### OrderManager

Управляет жизненным циклом собственных ордеров.

Целевая ответственность:

- принимать intents `SubmitLimit`, `Cancel`, `CancelAll` и `Replace`;
- назначать order ids;
- применять risk gates: max inventory, min quantity, tick alignment,
  maker-only behavior, если включено;
- писать artifacts жизненного цикла, например `orders.csv`.

### FillModel

Симулирует исполнение активных собственных ордеров.

MVP rule:

- buy limit исполняется, если `fill_reference <= limit_price`;
- sell limit исполняется, если `fill_reference >= limit_price`;
- fill price равен limit price ордера;
- partial fills по умолчанию выключены.

Планируемые references: `trade_price`, `best_quote`, `mid_price`. Fees задаются
через maker/taker basis points.

### Portfolio

Отслеживает cash, position, realized/unrealized PnL и fees.

Текущая ответственность:

- детерминированно применять fills;
- считать mark-to-market equity по текущему market state;
- отдавать inventory и cash для strategy risk limits и metrics.
- поддерживать long, short и reversal accounting через average entry price.

### MetricsEngine

Агрегирует run-level outputs:

- final PnL;
- mean/max inventory;
- turnover by quantity and notional;
- fill count и fill rate;
- drawdown;
- quoted spread и spread captured;
- adverse-selection metrics на заданных горизонтах.

Текущие artifacts: `metrics.json`, `equity_curve.csv`, `inventory.csv`,
`orders.csv`, `fills.csv`.

## 5. Конфигурация

Конфиги лежат в `lob_backtester/configs/`.

Текущая схема:

```yaml
run:
  symbol: BTCUSDT
  input_path: data/sample
  output_dir: lob_backtester/artifacts/runs/example

market:
  tick_size: 0.01
  lot_size: 0.000001

book:
  max_depth: 50

execution:
  fill_model: price_cross
  fill_reference: trade_price
  partial_fills: false
  maker_bps: 0.0
  taker_bps: 0.0

strategy:
  name: noop
  gamma: 0.1
  sigma: 0.02
  k: 1.5
  horizon_seconds: 3600.0
  sigma_window_ms: 1000
  min_spread_ticks: 2
  fair_price_mode: mid
  microprice_alpha: 1.0
  microprice_beta: 1.0
  delta_ticks: 1
  order_qty: 1
  max_inventory: 10
  quote_refresh_ms: 100
```

Для fixed-spread baseline используется `strategy.name: fixed_spread`; CLI также
поддерживает aliases `baseline_fixed` и `fixed`. Для классической модели
используется `strategy.name: avellaneda_stoikov`; aliases: `avellaneda`, `as`.
Для microprice extension используется `strategy.name: microprice_as`;
поддерживаются aliases `microprice_avellaneda_stoikov` и `mp_as`, а
`fair_price_mode` должен быть `microprice_proxy`.
`quote_refresh_ms` остается engine scheduler параметром, а `max_inventory`
дополнительно прокидывается в OMS risk gate. Будущие задачи расширят схему
настройками DataLoader, strategy-specific секциями, reporting settings, CLI
overrides и run metadata.

## 6. Модели стратегий

### Fixed Spread Baseline

`FixedSpreadStrategy` является sanity baseline. На каждом scheduled callback
она генерирует:

```text
cancel_all(strategy_id)
bid = floor(mid - delta_ticks)
ask = ceil(mid + delta_ticks)
```

Bid не выставляется, если возможное исполнение превысит long-side
`max_inventory`; ask не выставляется, если возможное исполнение превысит
short-side лимит. Если mid отсутствует или quote prices невалидны, стратегия
только снимает старые заявки. CLI включает maker-only risk gate для этой
стратегии.

### Avellaneda-Stoikov

Классическая стратегия строит quotes вокруг reservation price:

```text
r_t = mid - q * gamma * sigma^2 * (T - t)
```

Total spread:

```text
psi_t = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
```

Quotes округляются к tick size:

```text
bid = round_down(r_t - psi_t / 2, tick_size)
ask = round_up(r_t + psi_t / 2, tick_size)
```

Inventory `q` смещает reservation price. Положительный inventory должен
двигать reservation price вниз, отрицательный inventory - вверх.

Текущая реализация:

- `AvellanedaStoikovStrategy` на каждом scheduled callback сначала снимает
  старые заявки через `cancel_all`, затем строит новые maker quotes.
- `sigma_window_ms` задает time-window rolling std по mid-returns; до
  накопления двух returns используется стартовое значение `sigma` из YAML.
- Rolling return std переводится в tick-volatility через умножение на текущий
  mid, чтобы формула работала в price-tick units.
- `horizon_seconds` интерпретируется как оставшееся время от первого
  strategy callback; после истечения горизонта inventory term становится нулем.
- `min_spread_ticks`, maker-only checks и `max_inventory` защищают от crossed
  quotes и неконтролируемого inventory.

### Microprice Extension

Microprice variant корректирует fair price через imbalance:

```text
r_t = mid + beta * (microprice - mid) - q * gamma * sigma^2 * (T - t)
```

В MVP microprice - proxy на основе top-of-book imbalance. Learned microprice
table является stretch goal и должен оставаться в roadmap, если не будет
реализован отдельной задачей.

Реализация T11 переиспользует `AvellanedaStoikovStrategy` с режимом
`FairPriceMode::MicropriceProxy`:

- `microprice_proxy = mid + microprice_alpha * (spread / 2) * imbalance`;
- `fair_price = mid + microprice_beta * (microprice_proxy - mid)`;
- при `microprice_beta = 0` стратегия численно совпадает с classic A-S на том
  же потоке состояний.

## 7. Build, Test, Run

Из корня репозитория:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lob_backtest --config lob_backtester/configs/example.yaml
./build/lob_backtest --config lob_backtester/configs/baseline_fixed_spread.yaml
./build/lob_backtest --config lob_backtester/configs/avellaneda_stoikov.yaml
./build/lob_backtest --config lob_backtester/configs/microprice_as.yaml
```

Проверка форматирования:

```bash
find lob_backtester/apps lob_backtester/src lob_backtester/tests -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format --dry-run --Werror
```

Python helper environment:

```bash
python3.11 -m venv lob_backtester/scripts/python/.venv
source lob_backtester/scripts/python/.venv/bin/activate
pip install -r lob_backtester/scripts/python/requirements.txt
```

## 8. Данные и artifacts

Raw market data хранится только локально. Не добавляй скачанные архивы и
распакованные файлы в git.

Планируемая структура data:

```text
data/
  MD.zip
  raw/
  sample/
```

Планируемая структура run artifacts:

```text
artifacts/runs/<run_name>/
  run_metadata.json
  metrics.json
  orders.csv
  fills.csv
  equity_curve.csv
  inventory.csv
```

`docs/data_audit.md` фиксирует схему данных, единицу timestamp, типы колонок,
sample interval и решение по preprocessing. MVP читает CSV напрямую; binary
preprocessing откладывается до replay benchmark.

## 9. Стратегия тестирования

Тестирование должно следовать границам задач из `docs/implementation_plan.md`.

Минимальное ожидаемое покрытие по зонам:

- config parsing smoke tests;
- DataLoader parser и timestamp-order tests;
- OrderBook invariants и crossed-book handling;
- FeatureEngine numerical tests;
- OMS lifecycle и risk-gate tests;
- FillModel buy/sell crossing tests;
- Portfolio accounting tests;
- BacktestEngine synthetic integration, look-ahead и sample replay throughput
  tests;
- strategy formula tests;
- integration tests на synthetic streams и позже на `data/sample/`.

Synthetic tests предпочтительны для детерминированных edge cases. Real sample
data используется для integration и throughput checks после T1.

## 10. Roadmap

Ближайшие задачи:

1. T12: CLI/config polish.
2. T13-T14: dashboard, эксперименты, отчет и финальная
   полировка.

Финальные deliverables:

- runnable CLI backtester;
- configs для baseline, Avellaneda-Stoikov и microprice A-S;
- tests и sample data;
- experiment artifacts;
- final report и reproducibility instructions.
