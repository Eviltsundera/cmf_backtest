# AGENTS.md

## Контекст репозитория

Этот репозиторий содержит CMF LOB backtester для вступительного HFT-задания.
Целевая реализация: event-driven C++20 backtester для исторического replay
лимитной книги, стратегии Avellaneda-Stoikov и microprice extension.

Главный источник планирования:

- `docs/implementation_plan.md`

Перед выбором следующей задачи читай этот файл. Когда задача или DoD-пункт
закрыты, обновляй статус в плане.

## Структура проекта

- `lob_backtester/` - C++ проект backtester-а.
- `lob_backtester/src/lob/` - доменные модули ядра.
- `lob_backtester/apps/lob_backtest.cpp` - CLI entry point.
- `lob_backtester/configs/` - YAML-конфиги запусков.
- `lob_backtester/tests/` - GoogleTest тесты.
- `lob_backtester/scripts/python/` - audit данных, графики, dashboard и
  report helpers.
- `docs/` - описание задания, implementation plan, архитектура и техническая
  документация.
- `data/` - только локальные market data. Не коммить raw datasets и
  распакованные файлы.

## Сборка и проверка

Команды выполнять из корня репозитория:

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

Если зависимости еще не скачаны, CMake подтягивает `yaml-cpp`, `spdlog` и
`googletest` через `FetchContent`.

## Правила разработки

- Сохраняй текущую C++20/CMake структуру.
- Держи небольшие доменные модули в `src/lob/{data,book,features,execution,
  portfolio,strategies,engine,metrics,utils}`.
- Engine должен оставаться event-driven: market event, book update, fills,
  strategy, order management, portfolio, metrics.
- Не допускай look-ahead bias: стратегия видит только состояние, полученное из
  уже примененных событий.
- Добавляй сфокусированные тесты вместе с кодом, особенно для parsing, book
  invariants, fills, accounting и strategy formulas.
- Не добавляй в git raw data, build output, generated reports и локальные
  virtualenvs, если конкретная задача явно не требует обратного.

## Текущее состояние

T0 bootstrap, T1 data audit, T2 DataLoader, T3 LOBBuilder, T4 FeatureEngine,
T5 OMS, T6 FillModel, T7 Portfolio/MetricsEngine, T8 Engine integration, T9
FixedSpreadStrategy, T10 AvellanedaStoikovStrategy и T11 Microprice extension
закрыты. Сейчас в
репозитории есть:

- CMake project с `lob_core`, `lob_backtest` и `lob_tests`.
- YAML config loader в `lob::utils`.
- Streaming CSV DataLoader в `lob::data`.
- Market-By-Price `OrderBook` в `lob::book`.
- Stateless order-book features и rolling volatility в `lob::features`.
- OrderManager lifecycle/risk gates и FillModel в `lob::execution`.
- Portfolio accounting в `lob::portfolio`.
- MetricsEngine и вывод `metrics.json`, `equity_curve.csv`, `inventory.csv` в
  `lob::metrics`.
- Strategy interface, `NoopStrategy`, `FixedSpreadStrategy`,
  `AvellanedaStoikovStrategy` с classic/microprice fair-price modes и
  `BacktestEngine` event loop в
  `lob::strategies`/`lob::engine`.
- CLI replay path: `lob_backtest --config <yaml>` с `strategy.name: noop`,
  `fixed_spread`, `avellaneda_stoikov` или `microprice_as`.
- Baseline configs: `lob_backtester/configs/baseline_fixed_spread.yaml`,
  `lob_backtester/configs/avellaneda_stoikov.yaml`,
  `lob_backtester/configs/microprice_as.yaml`.
- Run artifacts: `metrics.json`, `equity_curve.csv`, `inventory.csv`,
  `orders.csv`, `fills.csv`.
- Example config, config smoke test и DataLoader tests.
- `docs/data_audit.md` со схемой `MD.zip`.
- `lob_backtester/scripts/python/audit.py` для аудита raw CSV и регенерации
  `data/sample/`.

Следующая задача по плану: T12, CLI и конфигурация.
