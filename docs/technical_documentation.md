# Техническая документация: LOB Backtester

**Статус:** начальный draft, синхронизирован с T0 bootstrap.

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

Сейчас кодовая база находится на уровне T0 bootstrap:

- `lob_backtester/CMakeLists.txt` определяет `lob_core`, `lob_backtest` и
  `lob_tests`.
- `lob_backtester/src/lob/utils/Config.hpp` и `.cpp` загружают YAML-конфиг в
  типизированные структуры.
- `lob_backtester/apps/lob_backtest.cpp` парсит `--config`, загружает YAML,
  логирует путь, печатает разобранные параметры и завершает работу.
- `lob_backtester/configs/example.yaml` содержит smoke config.
- `lob_backtester/tests/config_smoke_test.cpp` проверяет загрузку example
  config.

Текущий CLI намеренно является smoke path. Data parser, order book, execution
simulator, portfolio, metrics и strategies еще не реализованы.

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

Целевая ответственность:

- детерминированно применять fills;
- считать mark-to-market equity по текущему market state;
- отдавать inventory и cash для strategy risk limits и metrics.

### MetricsEngine

Агрегирует run-level outputs:

- final PnL;
- mean/max inventory;
- turnover by quantity and notional;
- fill count и fill rate;
- drawdown;
- quoted spread и spread captured;
- adverse-selection metrics на заданных горизонтах.

Планируемые artifacts: `metrics.json`, `equity_curve.csv`, `inventory.csv`,
`orders.csv`, `fills.csv`.

## 5. Конфигурация

Конфиги лежат в `lob_backtester/configs/`.

Текущая схема:

```yaml
run:
  symbol: BTCUSDT
  input_path: ../data/sample
  output_dir: artifacts/runs/example

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
  name: avellaneda_stoikov
  gamma: 0.1
  sigma: 0.02
  k: 1.5
```

Будущие задачи расширят схему настройками DataLoader, strategy-specific
секциями, reporting settings, CLI overrides и run metadata.

## 6. Модели стратегий

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

### Microprice Extension

Microprice variant корректирует fair price через imbalance:

```text
r_t = mid + beta * (microprice - mid) - q * gamma * sigma^2 * (T - t)
```

В MVP microprice - proxy на основе top-of-book imbalance. Learned microprice
table является stretch goal и должен оставаться в roadmap, если не будет
реализован отдельной задачей.

## 7. Build, Test, Run

Из корня репозитория:

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lob_backtest --config lob_backtester/configs/example.yaml
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
- strategy formula tests;
- integration tests на synthetic streams и позже на `data/sample/`.

Synthetic tests предпочтительны для детерминированных edge cases. Real sample
data используется для integration и throughput checks после T1.

## 10. Roadmap

Ближайшие задачи:

1. T2: реализовать normalized market-event loading.
2. T3: реализовать Market-By-Price order book.
3. T4: реализовать order-book features и rolling volatility.
4. T5-T8: добавить OMS, fills, portfolio, metrics и event loop.
5. T9-T14: стратегии, конфиги, dashboard и финальные эксперименты.

Финальные deliverables:

- runnable CLI backtester;
- configs для baseline, Avellaneda-Stoikov и microprice A-S;
- tests и sample data;
- experiment artifacts;
- final report и reproducibility instructions.
