# План реализации: LOB Backtest Engine + Avellaneda–Stoikov / Microprice

**Версия:** 1.0
**Цель:** доставить вступительный проект — event-driven backtest engine на C++ для исторических данных лимитной книги, с реализацией стратегий Avellaneda–Stoikov (2008) и Microprice + A–S (2018), отчётом и документацией.

**Технологический стек:**
- **Ядро:** C++20, CMake 3.20+, GoogleTest, clang-format/clang-tidy.
- **Зависимости:** `yaml-cpp` (конфиги), `spdlog` (логи), стандартный CSV-парсер (свой, минимум аллокаций), опционально `Apache Arrow` если данные в Parquet.
- **Обвязка:** Python 3.11 (только пост-обработка: `pandas`, `matplotlib` для графиков и финального отчёта). Никакого `pybind11` в MVP — обмен через CSV/JSON артефакты.
- **Расположение:** [cmf/lob_backtester/](../lob_backtester/).
- **Данные:** [cmf/data/raw/](../data/raw/) (распакованный `MD.zip`), sample → [cmf/data/sample/](../data/sample/).

**Принципы:**
- Каждая таска имеет явное **Definition of Done (DoD)** — без выполнения всех пунктов таска не закрывается.
- Тесты и acceptance scenario пишутся ВМЕСТЕ с кодом, не после.
- `master`/`main` всегда зелёный: после каждой таски `cmake --build && ctest` проходят.
- В отчёте явно фиксируем все упрощения MVP.

**Упрощения MVP (зафиксировать в отчёте):**
- Market-By-Price (агрегированная книга), без queue position.
- Fill при пересечении рыночной цены с уровнем ордера, без partial fills.
- Цена исполнения = limit price (консервативно).
- Latency = 0 (но интерфейс параметризован).
- Только spot-инструменты, один символ за run.

---

## Карта зависимостей задач

```text
T0 ─┬─ T1 ── T2 ── T3 ── T4 ── T7 ── T8 ─┬─ T9  ── T11 ── T12 ── T13 ── T14
    │                  └─ T5 ── T6 ──────┘   └─ T10 ─┘
    └─ дата audit и bootstrap идут первыми; всё остальное следует за ними.
```

Критический путь: T0 → T1 → T2 → T3 → T7 → T8 → T10 → T11 → T13 → T14.

---

## T0. Bootstrap репозитория [done]

**Цель:** инициализировать C++ проект с CMake, тестами, линтерами, CLI-скелетом.

**Статус задачи:** done, выполнено и запушено в `main` на GitHub.

**Подзадачи:**
- Создать `cmf/lob_backtester/` со структурой:
  ```
  lob_backtester/
  ├── CMakeLists.txt
  ├── cmake/                  # find-модули, toolchain
  ├── src/lob/                # ядро (header + impl)
  │   ├── data/
  │   ├── book/
  │   ├── execution/
  │   ├── portfolio/
  │   ├── strategies/
  │   ├── engine/
  │   ├── metrics/
  │   └── utils/
  ├── apps/
  │   └── lob_backtest.cpp    # CLI
  ├── tests/
  ├── configs/
  ├── scripts/                # Python: audit, plotting, report
  ├── third_party/            # vendored deps если нужно
  └── README.md
  ```
- `CMakeLists.txt`: target `lob_core` (static lib), `lob_backtest` (executable), `lob_tests` (gtest).
- Подключить `yaml-cpp`, `spdlog`, `gtest` через `FetchContent` (без системных зависимостей).
- `.clang-format` (LLVM-стиль, 100 cols), `.clang-tidy` (минимум: `bugprone-*`, `performance-*`).
- `scripts/python/` со своим `pyproject.toml` и `requirements.txt` (pandas, matplotlib, pyarrow).
- CLI-скелет: `lob_backtest --config <yaml>` парсит YAML и логирует hello-world.
- `README.md` — как собрать и запустить.

**DoD:**
- [x] `cmake -S cmf/lob_backtester -B build && cmake --build build -j` собирается без warning'ов.
- [x] `ctest --test-dir build` запускает 1 пустой тест и зелёный.
- [x] `./build/lob_backtest --config cmf/lob_backtester/configs/example.yaml` печатает разобранные параметры и выходит с кодом 0.
- [x] `clang-format --dry-run --Werror` чистый на всех файлах.
- [x] `README.md` содержит команды build/run.

**Статус на 2026-05-08:** реализация bootstrap добавлена:
`lob_backtester/CMakeLists.txt`, `lob_core`, `lob_backtest`, `lob_tests`,
пример YAML-конфига, `README.md`, `.clang-format`, `.clang-tidy` и Python
script scaffold. DoD проверен через временный toolchain в `/tmp/cmf-verify-venv`:
`cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release`,
`cmake --build build -j`, `ctest --test-dir build --output-on-failure`,
`./build/lob_backtest --config lob_backtester/configs/example.yaml`,
`clang-format --dry-run --Werror`.

---

## T1. Data audit и определение схемы

**Цель:** понять формат `MD.zip`, зафиксировать схему событий, выделить sample.

**Подзадачи:**
- Распаковать `cmf/data/MD.zip` в `cmf/data/raw/`.
- `lob_backtester/scripts/python/audit.py` — читает первые N строк/файлов, печатает:
  - имена файлов, форматы (CSV/parquet/jsonl), размеры;
  - имена колонок, типы, диапазон значений;
  - единицы timestamp (ns/us/ms/s), наличие `sequence`, наличие `side` для trades;
  - количество событий по типам (snapshot/depth_update/trade);
  - временной диапазон.
- Записать результаты в [cmf/docs/data_audit.md](data_audit.md): схема, упрощающие предположения, фиксированный tick_size и lot_size, символ.
- Выделить **sample** ~1 час самого активного интервала в `cmf/data/sample/` (для unit/integration тестов и быстрых прогонов).
- Решить: парсим CSV/JSONL напрямую в C++ или конвертируем в бинарный формат (Apache Arrow IPC / простой packed binary) на стадии preprocessing для скорости replay.

**DoD:**
- [x] `cmf/docs/data_audit.md` содержит таблицу колонок и пример записи каждого типа события.
- [x] `cmf/data/sample/` существует и содержит файл(ы) <50MB с покрытием все типов событий.
- [x] `lob_backtester/scripts/python/audit.py` идемпотентен и запускается одной командой.
- [x] Принято и задокументировано решение по preprocessing (raw → in-engine binary либо raw напрямую).

**Статус:** done 2026-05-08.

Итог: `MD.zip` содержит CSV-only данные: `lob.csv` как 25-level book snapshots и
`trades.csv` как trade stream. Native `depth_update` отсутствует. Sample выбран
по самому активному часу `2024-08-05 06:00:00..07:00:00 UTC`; файлы sample
меньше 50MB. Решение по preprocessing: в MVP парсим CSV напрямую в C++ и
возвращаемся к binary format только если replay benchmark покажет bottleneck.

---

## T2. DataLoader: парсинг и нормализация event stream [done]

**Цель:** читать сырые данные и отдавать унифицированный поток `MarketEvent` в порядке времени.

**Подзадачи:**
- Определить:
  ```cpp
  struct MarketEvent {
      int64_t ts_ns;
      uint64_t seq;
      EventType type;        // SNAPSHOT, DEPTH_UPDATE, TRADE
      // POD payload по типу:
      union { SnapshotPayload snap; DepthUpdatePayload du; TradePayload tr; };
  };
  ```
- `IDataSource` интерфейс с `bool next(MarketEvent&)`.
- Реализации: `CsvDataSource` (стриминг, без загрузки всего в память), при необходимости `BinaryDataSource`.
- Сортировка по `(ts, seq)`: либо данные уже отсортированы (проверить в audit), либо k-way merge при чтении нескольких файлов.
- Валидация: монотонность ts, отсутствие дублей, корректность tick alignment.
- Округление цены к `tick_size`, объёма к `lot_size` (конфигурируемо).

**DoD:**
- [x] Юнит-тесты на парсинг каждого типа события.
- [x] Юнит-тест на детекцию нарушения монотонности `ts`.
- [x] Юнит-тест на k-way merge при нескольких источниках.
- [x] Интеграционный тест: на sample проходит весь файл без ошибок и количество событий соответствует ожиданию из audit.
- [x] Замер пропускной способности (events/sec) на sample, зафиксирован в [data_audit.md](data_audit.md).

**Статус:** done 2026-05-08.

**Итог:** добавлены `MarketEvent`, `IDataSource` и streaming `CsvDataSource`.
Loader читает `lob.csv`, `trades.csv` и опциональный `depth_updates.csv`,
нормализует timestamps в ns, prices в ticks, quantities в lots и делает merge
по `(ts_ns, seq)` без загрузки всего датасета в память. На sample
интеграционный тест прошел `757,667` событий; измеренная пропускная способность:
`3,341,723 events/sec`.

---

## T3. LOBBuilder: реконструкция книги (Market-By-Price) [done]

**Цель:** поддерживать актуальную агрегированную лимитную книгу.

**Статус:** done 2026-05-08.

**Подзадачи:**
- `OrderBook`:
  - `bids: std::map<Price, Qty, std::greater<>>` (или `boost::container::flat_map` для cache locality);
  - `asks: std::map<Price, Qty>`.
  - API: `apply_snapshot`, `apply_update(side, price, size)`, `best_bid()`, `best_ask()`, `mid()`, `spread()`, `level(side, depth)`.
- Удаление уровня при `size == 0`.
- Защита от crossed/locked book: логирование + опциональная политика recovery (drop bid/ask до устранения, либо ignore с warning).
- Ограничение хранимой глубины `max_depth` из конфига.
- Версионирование snapshot — `snapshot_id` для walk-forward проверок.

**DoD:**
- [x] Юнит-тесты:
  - apply_snapshot восстанавливает книгу из 0;
  - apply_update добавляет/обновляет/удаляет уровень;
  - после серии операций `best_bid < best_ask`;
  - детекция crossed book, политика recovery срабатывает.
- [x] Property-based тест (или fuzz): применение случайных update'ов не приводит к UB и сохраняет инвариант.
- [x] Бенчмарк `apply_update`: ≥ 1M ops/sec на одном ядре (sanity для replay).

**Итог:** добавлен `lob::book::OrderBook` для Market-By-Price книги с bid/ask
maps, `apply_snapshot`, `apply_update`, `apply_event`, top-of-book API,
`mid`, `spread`, доступом к уровням, `max_depth`, `snapshot_id` и политиками
`Reject`, `DropCrossingLevels`, `IgnoreWithWarning` для locked/crossed book.
Тесты покрывают snapshot rebuild, update add/change/delete, event application,
strict `best_bid < best_ask`, recovery/rollback policy и deterministic fuzz.
Release benchmark на локальной сборке: `18,895,363 apply_update ops/sec`.

---

## T4. FeatureEngine: order-book признаки [done]

**Цель:** считать `mid`, `spread`, `imbalance`, `weighted_mid`, microprice proxy — отдельно от стратегии для переиспользования.

**Статус:** done 2026-05-08.

**Подзадачи:**
- Stateless функции над `OrderBook`:
  - `mid = (best_bid + best_ask) / 2`;
  - `spread = best_ask - best_bid`;
  - `imbalance = (V_b - V_a) / (V_b + V_a)` (top-of-book);
  - `weighted_mid = (a*V_b + b*V_a) / (V_b + V_a)`;
  - `microprice_proxy = mid + alpha * (spread/2) * imbalance`.
- Rolling volatility estimator (`RollingStd`) на mid-returns с конфигурируемым окном (по событиям ИЛИ по времени).
- Опциональный экспортер `book_features.csv` для sanity check / Python-аналитики.

**DoD:**
- [x] Юнит-тесты на каждую функцию (включая edge cases: одна сторона пуста, нулевые объёмы).
- [x] Тест на rolling σ: на синтетических данных совпадает с numpy std в пределах 1e-9.
- [x] При `alpha=0` microprice_proxy ≡ mid; при `alpha=1, V_b=V_a` ≡ mid.

**Итог:** добавлен модуль `lob::features` с stateless функциями `mid`,
`spread`, `imbalance`, `weighted_mid`, `microprice_proxy`, generic
`RollingStd` и `RollingMidReturnStd` для простой доходности mid-price.
Feature functions возвращают `std::nullopt`, если top-of-book неполный; нулевой
размер уровня в текущем `OrderBook` трактуется как удаление уровня. Опциональный
CSV-exporter отложен до появления engine loop / артефактов sanity-check.

---

## T5. OMS (OrderManager): жизненный цикл собственных ордеров [done]

**Цель:** принимать `OrderIntent` от стратегии и трансформировать в активные ордера, обрабатывать cancel/replace, fill.

**Статус:** done 2026-05-08.

**Подзадачи:**
- Типы:
  ```cpp
  enum class OrderStatus { ACTIVE, FILLED, CANCELLED, REJECTED };
  struct Order { OrderId id; Side side; Price price; Qty qty; Qty remaining; OrderStatus status; int64_t ts_submit; };
  ```
- `OrderIntent`: `SubmitLimit`, `Cancel`, `CancelAll(strategy_id)`, `Replace`.
- Risk gates перед активацией: `max_inventory`, `min_qty`, tick alignment, не пересекать рынок если `strict_maker=true`.
- Логи: `orders.csv` с полным жизненным циклом.

**DoD:**
- [x] Юнит-тесты:
  - submit добавляет ордер, status=ACTIVE;
  - cancel переводит в CANCELLED;
  - rejected при нарушении risk gate (превышение max_inventory);
  - replace = cancel + submit, новый id.
- [x] Тест: после fill ордер уходит из active store, статус FILLED.

**Итог:** добавлен `lob::execution::OrderManager` с типами `Order`,
`OrderIntent`, `OrderStatus`, lifecycle event log и active-order store. OMS
поддерживает submit/cancel/cancel_all/replace/fill, сохраняет историю статусов
после удаления из active set и пишет `orders.csv`. Risk gates покрывают
`max_inventory`, `min_qty`, tick alignment и `strict_maker` against current
top-of-book. Fill logic пока только lifecycle transition; условия исполнения
остаются задачей T6.

---

## T6. FillModel: симулятор исполнения [done]

**Цель:** определять, исполнились ли активные ордера на текущем market event'е.

**Статус:** done 2026-05-08.

**Подзадачи:**
- Правила (конфигурируемо):
  - `fill_reference = trade_price | best_quote | mid_price`;
  - buy fill: `ref <= limit`; sell fill: `ref >= limit`;
  - fallback на best_quote если событие — depth update без trade.
- `fill_price = limit_price` (консервативно).
- Без partial fills в MVP (флаг в конфиге для будущего).
- Поддержка fees: `maker_bps`, `taker_bps`, `default_role`.
- Latency model заглушкой (поля есть, обработка = noop).

**DoD:**
- [x] Юнит-тесты:
  - buy limit при trade_price <= limit → fill;
  - buy limit при trade_price > limit → нет fill;
  - симметричные кейсы для sell;
  - fallback на best_quote при отсутствии trade.
- [x] Интеграционный тест: синтетический сценарий «bid=99, ask=101, trade@99 → buy filled; trade@101 → sell filled; inventory=0».
- [x] При `fees=0` round-trip fills дают `pnl = (sell_px - buy_px) * qty` точно.

**Итог:** добавлен `lob::execution::FillModel` с configurable
`fill_reference` (`trade_price`, `best_quote`, `mid_price`), directional
fill-rules для buy/sell, fallback на best quote для non-trade events,
conservative `fill_price = limit_price`, maker/taker fees и latency noop field.
`fill_active_orders` работает поверх `OrderManager`, возвращает fills и
переводит исполненные ордера в `FILLED`. Partial fills пока не включены в MVP.

---

## T7. Portfolio + MetricsEngine

**Цель:** учёт капитала, расчёт PnL/inventory/turnover и market-making метрик.

**Подзадачи:**
- `Portfolio`: cash, position, methods `apply_fill`, `equity(mark_price)`, `realized_pnl`, `unrealized_pnl`.
- `MetricsEngine` агрегирует по run'у:
  - `final_pnl, mean/max_inventory, inventory_std, turnover_qty, turnover_notional, fill_count, fill_rate, max_drawdown`;
  - market making специфика: `avg_quoted_spread`, `avg_spread_captured`, `adverse_selection_h` для нескольких горизонтов h, `quote_uptime`.
- Вывод: `metrics.json`, `equity_curve.csv`, `inventory.csv`.

**DoD:**
- [ ] Юнит-тесты на accounting: buy/sell, fees, mark-to-market.
- [ ] Sanity: no-trading run → PnL=0, turnover=0.
- [ ] Sanity: при `fees=0` и развороте по spread `pnl = spread * qty`.
- [ ] `metrics.json` содержит все поля из спецификации.

---

## T8. Engine: event loop и интеграция

**Цель:** склеить DataLoader → LOBBuilder → FeatureEngine → Strategy → OMS → FillModel → Portfolio → Metrics.

**Подзадачи:**
- `Strategy` интерфейс:
  ```cpp
  class IStrategy {
  public:
      virtual std::vector<OrderIntent> on_market_event(const MarketEvent&, const MarketState&) = 0;
      virtual std::vector<OrderIntent> on_fill(const Fill&, const MarketState&) = 0;
  };
  ```
- Жёсткий порядок в loop:
  1. apply event к OrderBook;
  2. check fills для активных ордеров;
  3. notify strategy через `on_fill`;
  4. опросить strategy через `on_market_event` (по timer/throttle);
  5. process intents в OMS.
- Scheduler: вызов стратегии не на каждом event'е, а по `quote_refresh_ms` (конфигурируемо).
- Защита от look-ahead: стратегия видит только уже применённые события.

**DoD:**
- [ ] Интеграционный тест на синтетическом событийном потоке: dummy strategy выставляет фиксированные quotes, fills и portfolio проверяются в конце.
- [ ] Тест на отсутствие look-ahead: стратегия не получает данные будущего события на текущем шаге.
- [ ] Бенчмарк replay sample-а: события/сек, время на event.

---

## T9. Naive baseline strategy: fixed spread

**Цель:** простейшая стратегия для sanity-тестов и сравнения.

**Подзадачи:**
- `FixedSpreadStrategy`: cancel_all → submit bid=mid-Δ, ask=mid+Δ каждые `quote_refresh_ms`.
- Конфиг: `delta_ticks`, `order_qty`, `quote_refresh_ms`, `max_inventory`.

**DoD:**
- [ ] Юнит-тест на генерацию intents: ровно cancel_all + 1 buy + 1 sell.
- [ ] Прогон на sample → `reports/baseline_fixed/` создан, метрики не NaN.
- [ ] При `fees=0` и mean-reverting синтетике стратегия зарабатывает положительный PnL.

---

## T10. Avellaneda–Stoikov (2008)

**Цель:** реализовать классическую модель.

**Подзадачи:**
- Volatility estimator: rolling std mid-returns, окно из конфига.
- Расчёт:
  - `r_t = mid - q * γ * σ² * (T - t)`;
  - `ψ_t = γ * σ² * (T - t) + (2/γ) * ln(1 + γ/k)`;
  - `bid = round_down(r_t - ψ/2, tick); ask = round_up(r_t + ψ/2, tick)`.
- Guard: `min_spread_ticks`, не выставлять crossed quotes, ограничение по `max_inventory` (стопаем bid при достижении лимита long, и наоборот).
- Параметры: `gamma`, `k`, `horizon_seconds`, `sigma_window_ms`, `order_qty`.

**DoD:**
- [ ] Юнит-тесты на формулы: при `q=0` reservation = mid; при `q>0` reservation < mid; при `q<0` > mid.
- [ ] При росте `γ` ИЛИ `σ` total spread растёт.
- [ ] При `(T-t)→0` total spread → `(2/γ)*ln(1+γ/k)` (без inventory term).
- [ ] Прогон на sample → `reports/avellaneda_stoikov/`.

---

## T11. Microprice extension (A–S 2018)

**Цель:** улучшить fair price через imbalance/microprice.

**Подзадачи:**
- Microprice proxy уже есть из T4.
- Reservation формула с `β`:
  - `r_t = mid + β * (mp - mid) - q * γ * σ² * (T - t)`.
- Опциональный bin-based microprice (на потом, в roadmap, не в MVP):
  - дискретизация `(imbalance, spread)` → таблица условных ожиданий.
- Конфиг: `microprice_alpha`, `microprice_beta`, `fair_price_mode`.

**DoD:**
- [ ] При `β=0` стратегия численно совпадает с A–S (юнит-тест на одинаковом потоке).
- [ ] При `imbalance > 0` (bid-heavy) reservation > классический A–S при том же `q`.
- [ ] При `imbalance < 0` (ask-heavy) reservation < классический A–S.
- [ ] Прогон на sample → `reports/microprice_as/`.

---

## T12. CLI и конфигурация

**Цель:** удобный запуск экспериментов через YAML.

**Подзадачи:**
- `lob_backtest --config <path> [--override key=value ...]`.
- Конфиги в `cmf/lob_backtester/configs/`:
  - `baseline_fixed_spread.yaml`;
  - `avellaneda_stoikov.yaml`;
  - `microprice_as.yaml`.
- Структура YAML — как в [backtest_engine_plan_avellaneda_stoikov.md §8.1](backtest_engine_plan_avellaneda_stoikov.md).
- Логи: уровень из конфига, JSON output для машинного парсинга.
- `output_dir/run_metadata.json`: hash конфига, git commit, timestamp.

**DoD:**
- [ ] Все 3 конфига валидны и запускаются на sample.
- [ ] CLI override работает: `--override strategy.gamma=0.05` меняет параметр.
- [ ] `run_metadata.json` создаётся в output dir и содержит config hash + commit.

---

## T13. Reporting layer (Streamlit dashboard)

**Цель:** интерактивный дашборд для исследования run'ов глазами + текстовый экспорт для submission.

**Стек:** `streamlit` + `plotly` + `pandas`. Запуск: `streamlit run scripts/python/dashboard.py -- --reports-dir reports/`.

**Подзадачи:**
- `scripts/python/dashboard.py` — главное приложение Streamlit.
- **Sidebar:**
  - мультиселект run'ов из `reports/` (читаются по списку поддиректорий с `metrics.json`);
  - переключатель «single run» / «compare runs»;
  - фильтр по временному окну (slider по `[ts_start, ts_end]` из equity_curve);
  - выбор инструментов сравнения (overlay/normalize).
- **Главные tabs:**
  1. **Overview** — таблица метрик (если несколько run'ов — wide-format side-by-side с подсветкой best/worst), `run_metadata.json` (config hash, git commit).
  2. **Equity & PnL** — Plotly line chart `equity_t` с overlay'ем нескольких run'ов; drawdown под графиком.
  3. **Inventory** — line chart `position_t` + гистограмма распределения; маркер `max_inventory` лимита.
  4. **Quoting** — quote distance to mid (bid/ask), avg quoted spread vs time, маркеры fill'ов.
  5. **Fills** — scatter `fill_price vs mid` с цветом по side; таблица fills с пагинацией.
  6. **Adverse selection** — bar chart по горизонтам `h ∈ {100ms, 1s, 10s}`.
  7. **Sensitivity** (если в директории есть grid run'ов) — Plotly heatmap по парам `(γ, β)`, `(γ, k)` и т.п. для PnL/Sharpe-like/turnover.
- **Экспорт для submission:** кнопка «Export static report» в Streamlit → генерирует `reports/_static/<run>/summary.md` + `plots/*.png` (через `plotly.io.write_image` + `kaleido`). Автоматически вызывается также из CLI: `python scripts/python/export_static.py --run <dir>` — нужно для submission package, чтобы ревьюеру не пришлось ставить Streamlit.
- **Cache:** `@st.cache_data` для чтения CSV — мгновенное переключение между tabs.
- `requirements.txt` обновить: `streamlit>=1.32`, `plotly>=5.20`, `kaleido` (для static export), `pandas`, `pyarrow`.

**DoD:**
- [ ] `streamlit run scripts/python/dashboard.py -- --reports-dir reports/` поднимает дашборд на localhost; все 7 tabs рендерятся без ошибок на 1+ run'е.
- [ ] Compare-mode корректно работает на 3+ run'ах: метрики side-by-side, overlay equity curves.
- [ ] Sensitivity tab корректно показывает heatmap при наличии grid-run'ов (например `reports/grid_gamma_beta/run_*/`).
- [ ] `python scripts/python/export_static.py --run reports/avellaneda_stoikov/` создаёт `summary.md` + `plots/*.png` без запуска Streamlit (для submission).
- [ ] README содержит секцию «Viewing results» с командой запуска и скриншотом дашборда.
- [ ] Дашборд работает на чистой машине после `pip install -r requirements.txt`.

---

## T14. Эксперименты и финальный отчёт

**Цель:** собрать результаты для сдачи.

**Подзадачи:**
- Запустить:
  - baseline_fixed_spread на полном датасете;
  - avellaneda_stoikov на полном датасете;
  - microprice_as на полном датасете;
  - sensitivity grid (3×3×3) по `(γ, k, β)` на репрезентативном subset'е.
- Walk-forward (если данных хватает): train 60% / val 20% / test 20%.
  - На train оценить `σ`, при необходимости `k`;
  - На val подобрать `γ`, `β`;
  - На test — финальные метрики.
- Финальный отчёт [cmf/docs/report.md](report.md):
  1. Постановка задачи.
  2. Архитектура engine (краткая, со ссылкой на этот план).
  3. Описание моделей (A–S и microprice extension) с формулами.
  4. Методология экспериментов и сетка параметров.
  5. Таблица результатов: PnL, max DD, mean inventory, turnover, fill rate, adverse selection — для baseline / A–S / Microprice-A–S.
  6. Графики: equity curves overlay, inventory distributions, sensitivity heatmap.
  7. Обсуждение: что работает, что нет, чувствительность к γ и β.
  8. **Раздел "Ограничения модели исполнения"** — явно перечислить упрощения.
  9. **Roadmap улучшений** (взять из секции 14 [backtest_engine_plan_avellaneda_stoikov.md](backtest_engine_plan_avellaneda_stoikov.md)).
  10. Инструкции по воспроизведению (commands).

**DoD:**
- [ ] Все 3+ прогона имеют артефакты в `reports/`.
- [ ] Sensitivity grid заполнен, есть heatmap.
- [ ] `cmf/docs/report.md` написан и содержит все 10 секций.
- [ ] Microprice-A–S в среднем по grid не хуже A–S по net PnL ИЛИ зафиксировано почему хуже (с предположением о причинах).
- [ ] README в [cmf/lob_backtester/](../lob_backtester/) обновлён: команды build, run, reproduce, ссылки на configs и отчёт.

---

## T15. Submission package

**Цель:** упаковать всё для сдачи.

**Подзадачи:**
- Чеклист из [backtest_engine_plan_avellaneda_stoikov.md §16](backtest_engine_plan_avellaneda_stoikov.md):
  - [ ] README с инструкцией запуска;
  - [ ] configs для всех 3 стратегий;
  - [ ] src/ с engine + strategies;
  - [ ] tests/ с unit+integration;
  - [ ] data/sample/;
  - [ ] reports/report.md;
  - [ ] orders.csv, fills.csv, equity_curve.csv, metrics.json для каждого run;
  - [ ] графики;
  - [ ] раздел про ограничения исполнения;
  - [ ] roadmap.
- Финальный smoke-тест: clone в tmp, build, run sample, отчёт собирается.

**DoD:**
- [ ] Все пункты submission checklist отмечены.
- [ ] Smoke-тест на чистой копии проходит.
- [ ] Заявка отправлена через форму из [task_description.md](task_description.md).

---

## Опциональные задачи (stretch goals)

Эти задачи **не блокируют сдачу MVP** и не входят в критический путь. Если они не реализованы — их место в roadmap-секции финального отчёта. Если реализованы — добавляются отдельным разделом «Расширения» в [report.md](report.md) и сравниваются с MVP-версией.

Рекомендуемый порядок реализации (по соотношению эффект/трудозатраты): O1 → O2 → O3 → O4.

---

### O1. Learned microprice (bin-based transition matrix)

**Цель:** заменить proxy-формулу microprice на эмпирическую оценку из работы Stoikov (2018).

**Зависимости:** T11 (Microprice extension) должен быть готов и работать с proxy.

**Подзадачи:**
- Оценщик `LearnedMicroprice`:
  - дискретизация по `(imbalance_bin, spread_in_ticks)`;
  - на train-сете накопить статистику переходов: `P(Δmid = +tick | bin)`, `P(Δmid = -tick | bin)`, `P(Δmid = 0 | bin)`;
  - итеративная оценка ожидаемого будущего mid: `mp = m + tick * E[Δmid_∞ | bin]` через fixed-point по transition matrix.
- Сериализация модели: `microprice_model.json` с биннингом и таблицей переходов.
- Подключение через `fair_price_mode: learned_microprice` в конфиге стратегии.
- Сравнение proxy vs learned в отчёте: какая лучше предсказывает mid на горизонтах 100ms / 1s / 10s.

**DoD:**
- [ ] `lob_microprice_train --data <train> --bins <config> --out microprice_model.json` создаёт модель.
- [ ] Юнит-тест: на синтетическом потоке с известным bias предсказание сходится к истинному ожиданию в пределах 1 tick.
- [ ] `microprice_as.yaml` поддерживает `fair_price_mode: learned_microprice` и грузит модель из файла.
- [ ] В отчёте есть таблица предсказательной силы: MSE(mp_learned vs mid_{t+h}) < MSE(mp_proxy vs mid_{t+h}) хотя бы на одном горизонте, ИЛИ зафиксировано почему нет.
- [ ] При отсутствии модели стратегия корректно фолбечится на proxy (warning в лог).

---

### O2. Queue position model

**Цель:** более реалистичная симуляция исполнения через оценку позиции в очереди на ценовом уровне.

**Зависимости:** T3 (LOBBuilder) и T6 (FillModel) — добавляется поверх.

**Подзадачи:**
- При `submit` запоминать `queue_pos = volume_ahead` на уровне (по агрегированной книге это объём, видимый в book на момент постановки).
- При каждом trade на этом уровне: `queue_pos -= traded_qty`. При cancel'ах на уровне (depth_update со снижением size) — пропорциональное уменьшение `queue_pos` (модель FIFO-ish).
- Fill условия:
  - buy limit: исполняется когда `queue_pos <= 0` И есть trade по цене ≤ limit;
  - симметрично для sell.
- Конфиг: `queue_model: none | aggregate_fifo | shadow`.
- Замер: насколько изменяется fill rate и adverse selection vs naive price_cross.

**DoD:**
- [ ] Тест: ордер, поставленный за `Q` объёма, не исполняется до того, как через уровень пройдут объёмы ≥ Q.
- [ ] Тест: `queue_model=none` ≡ старое поведение из T6 (бэккомпат).
- [ ] На реальном sample queue model даёт fill rate ≤ price_cross (логично, т.к. часть fill'ов отсеивается).
- [ ] В отчёте есть сравнение метрик A–S под `queue_model=none` vs `aggregate_fifo`.

---

### O3. Partial fills

**Цель:** разрешить исполнение части ордера за один event, согласно доступному объёму.

**Зависимости:** T5 (OMS), T6 (FillModel), T7 (Portfolio).

**Подзадачи:**
- Order: `remaining_qty` уже есть из T5 — обеспечить, что после fill'а статус → `PARTIALLY_FILLED` пока `remaining_qty > 0`, потом `FILLED`.
- Размер fill'а:
  - если есть trade event с `qty`: `fill_qty = min(remaining_qty, trade.qty * fraction)` (fraction = доля в нашу пользу, конфигурируемо, default `min(0.5, ...)` или 1.0 если queue model отсутствует);
  - при `queue_model=aggregate_fifo` (O2): `fill_qty = max(0, min(remaining_qty, trade.qty - queue_pos))`.
- Конфиг: `partial_fills: bool`, `partial_fill_fraction: float`.
- Логирование: каждый partial fill отдельной записью в `fills.csv`.

**DoD:**
- [ ] Юнит-тест: ордер на 10 лотов получает 3+7 partial fills, итоговый realized PnL и position корректны.
- [ ] Юнит-тест: `partial_fills=false` ≡ MVP-поведение (бэккомпат).
- [ ] Метрики обновлены: `partial_fill_count`, `avg_fills_per_order`.
- [ ] В отчёте sensitivity: full fills vs partial fills — как меняется fill rate / adverse selection / PnL.

---

### O4. Market impact (минимальная модель)

**Цель:** учесть влияние своих ордеров на market state — для крупных ордеров и для «правдивости» больших inventory limits.

**Зависимости:** T6 (FillModel), T8 (Engine).

**Подзадачи:**
- Простая модель temporary impact: после fill'а `qty` на нашей стороне «съедается» эквивалентный объём на соответствующем уровне book (виртуально, без изменения исторических данных).
- Permanent impact (опционально): сдвиг mid на `λ * sign(qty) * sqrt(qty/V)` для последующих оценок (Almgren–Chriss-like).
- Конфиг:
  ```yaml
  impact:
    model: none | temporary | temporary_plus_permanent
    lambda_temp: 0.0
    lambda_perm: 0.0
  ```
- Замер: при каких размерах `order_qty` impact начинает доминировать над spread capture.

**DoD:**
- [ ] Юнит-тест: при `model=none` поведение совпадает с MVP.
- [ ] Юнит-тест: при `temporary` impact уровень best на нашей стороне теряет объём после fill'а до прихода нового depth update.
- [ ] В отчёте sensitivity по `order_qty` с включённым impact: есть ожидаемый decay PnL при росте размера.

---

## Ориентировочный график

| День | Задачи |
|---|---|
| 1 | T0 + T1 |
| 2 | T2 + T3 |
| 3 | T4 + T5 + T6 |
| 4 | T7 + T8 |
| 5 | T9 + T10 |
| 6 | T11 + T12 |
| 7 | T13 + T14 (эксперименты, прогон) |
| 8 | T14 (отчёт) + T15 |

Буфер 1–2 дня на доработку отчёта и финальные прогоны.

---

## Точки принятия решений (открытые вопросы)

1. **Формат данных после T1.** Решено: `MD.zip` содержит CSV-only `lob.csv` и `trades.csv`; для MVP используем свой streaming CSV parser.
2. **DataLoader после T2.** Решено: C++ loader стримит CSV напрямую, делает k-way merge и хранит price/qty как integer ticks/lots.
3. **Preprocessing в бинарь.** Решено отложить: binary preprocessing добавлять только если T3 replay benchmark покажет bottleneck CSV-парсинга.
4. **Walk-forward.** Данные покрывают 2024-08-01..2024-08-06 UTC; train/validation/test split возможен, конкретные границы выбрать в T14.
5. **Bin-based microprice.** В MVP — proxy формулой. Bin-based — в roadmap.
