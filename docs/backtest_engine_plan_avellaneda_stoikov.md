# План реализации backtest-движка для LOB и стратегии Avellaneda–Stoikov / Microprice

Версия: 1.0
Язык документа: русский
Цель: подготовить технический план, архитектуру и модельную спецификацию для вступительного задания: разработка движка обратного тестирования, воспроизводящего исторические данные лимитной книги заявок и оценивающего эффективность market-making стратегии.

---

## 1. Краткое резюме

Нужно реализовать **event-driven backtest engine** для исторического воспроизведения данных лимитной книги заявок, размещения и отмены собственных лимитных ордеров, моделирования исполнения по правилу пересечения цены и расчета базовых метрик: **PnL, inventory, turnover**. В качестве стратегии требуется реализовать **Avellaneda–Stoikov (2008)**, затем улучшить ее через **microprice / order-book imbalance** по работе Stoikov (2018), провести эксперименты и оформить техническую документацию.

Рекомендуемый путь: **Python-first** для скорости разработки, тестируемости и удобной аналитики. Архитектуру следует спроектировать модульно, чтобы наиболее тяжелые части — LOB replay, matching/fill simulation, расчет признаков — можно было позднее ускорить через `numba`, `polars`, Cython, Rust/C++ extension или полностью перенести на C++.

Минимальный MVP должен включать:

1. Загрузку исторических событий: snapshot, depth update, trade, либо восстановление LOB из доступного формата.
2. Локальную модель LOB: best bid/ask, spread, mid, объемы на уровнях, проверка crossed/locked book.
3. Собственный order manager: submit/cancel/replace limit orders.
4. Fill simulator: исполнение, когда рыночная цена пересекает уровень ордера.
5. Portfolio/accounting: cash, position, realized/unrealized/net PnL, fees/rebates.
6. Strategy interface + реализацию Avellaneda–Stoikov.
7. Microprice extension: замена/коррекция mid-price через microprice и imbalance.
8. Набор экспериментов, конфигурации, отчет, документацию и roadmap.

---

## 2. Источники и исходные ссылки из задания

Ссылка из задания на данные раскрывается как Google Drive-файл `MD.zip`; сам просмотр файла может требовать входа в Google-аккаунт. Поэтому в плане закладывается универсальный загрузчик и этап data audit, а не фиксируется конкретная схема колонок до проверки архива.

Ссылка на форму подачи раскрывается как Google Forms.

Основные статьи:

- Marco Avellaneda, Sasha Stoikov, **“High-frequency trading in a limit order book”**, Quantitative Finance, 2008, Vol. 8, Issue 3, pp. 217–224, DOI `10.1080/14697680701381228`.
- Sasha Stoikov, **“The micro-price: a high-frequency estimator of future prices”**, Quantitative Finance, 2018, Vol. 18, Issue 12, pp. 1959–1966, DOI `10.1080/14697688.2018.1489139`.

Ссылки на источники вынесены в раздел [15. References](#15-references).

---

## 3. Требования и трактовка задания

### 3.1. Функциональные требования

| Блок | Требование | Трактовка для реализации |
|---|---|---|
| Historical replay | Воспроизведение исторических данных | Итерировать события в порядке timestamp, восстанавливать публичную LOB, передавать события стратегии |
| LOB simulation | Моделирование лимитной книги | Поддерживать агрегированную книгу по ценовым уровням; для MVP достаточно Market-By-Price |
| Limit orders | Размещение и отмена лимитных ордеров | Хранить собственные активные ордера отдельно от публичной книги; поддержать `submit`, `cancel`, `replace` |
| Execution | Моделирование исполнения | MVP: fill при пересечении рыночной цены с уровнем ордера; частичные исполнения необязательны |
| Metrics | PnL, inventory, turnover | Считать mark-to-market PnL, cash, position, traded volume/notional turnover, fill statistics |
| Strategy | Avellaneda–Stoikov 2008 | Реализовать расчет reservation price и optimal spread/quotes |
| Extension | Microprice + A–S 2018 | Использовать microprice/imbalance как fair price или краткосрочный predictive skew |
| Experiments | Симуляционные эксперименты | Baseline vs A–S vs Microprice-A–S, sensitivity по gamma/sigma/k/fees/latency |
| Deliverables | Код, данные, конфиги, отчет, документация | Репозиторий с CLI, YAML-конфигами, тестами, отчетом Markdown/PDF при необходимости |

### 3.2. Уточнение термина “оборот кадров”

В контексте торговых стратегий под **turnover** корректнее понимать не “оборот кадров”, а **торговый оборот**:

- `turnover_qty = sum(abs(fill_qty))`;
- `turnover_notional = sum(abs(fill_qty * fill_price))`;
- `turnover_ratio = turnover_notional / initial_capital` или `/ gross_exposure`, если нужен нормированный показатель.

### 3.3. Допущение об исполнении

В задании указано: **исполнение происходит, когда рыночная цена пересекает уровень ордера**.

Для MVP это можно задать как конфигурируемое правило:

```yaml
execution:
  fill_model: price_cross
  fill_reference: trade_price        # trade_price | best_quote | mid_price
  partial_fills: false
  queue_model: none
```

Базовое правило:

- Buy limit по цене `p`: исполняется, если `market_price <= p`.
- Sell limit по цене `p`: исполняется, если `market_price >= p`.

Если в данных есть сделки, лучше использовать `last_trade_price` с направлением сделки, если оно доступно. Если сделок нет, fallback — пересечение best quotes:

- Buy limit исполняется, если `best_ask <= p`.
- Sell limit исполняется, если `best_bid >= p`.

В отчете нужно явно указать, что это упрощенная модель без queue priority и без market impact. Позднее ее можно расширить до queue-position модели.

---

## 4. Архитектура backtest-движка

### 4.1. Почему event-driven, а не vectorized

Для LOB/HFT-задачи лучше использовать **event-driven** архитектуру. Векторизованный backtest хорошо подходит для свечных данных и простых стратегий, но плохо моделирует жизненный цикл ордеров, отмены, задержки, очередность событий, частичные исполнения и микроструктуру. Event-driven backtester обрабатывает события последовательно, как торговая система в live-среде: market data event → strategy decision → order event → execution/fill event → portfolio update.

Плюсы event-driven подхода:

- меньше риск look-ahead bias, потому что стратегия видит только уже наступившие события;
- легче переиспользовать интерфейсы для live trading;
- можно детально моделировать order lifecycle;
- естественно добавляются latency, fees, cancel/replace, queue model, risk limits.

Минусы:

- больше компонентов и выше риск багов;
- медленнее чисто векторизованных расчетов;
- требует хороших unit/integration тестов.

### 4.2. Общая схема компонентов

```mermaid
graph LR
    A[Raw historical data] --> B[DataLoader / Parser]
    B --> C[MarketEvent stream]
    C --> D[LOBBuilder]
    D --> E[MarketState]
    E --> F[FeatureEngine]
    F --> G[Strategy]
    G --> H[OrderManager]
    H --> I[ExecutionSimulator]
    I --> J[Portfolio / Accounting]
    J --> K[MetricsEngine]
    K --> L[ReportGenerator]
    I --> H
    C --> I
    E --> I
```

### 4.3. Компоненты

#### 4.3.1. DataLoader / Parser

Задача: читать исходные данные, нормализовать события и отдавать их в едином формате.

Возможные входные форматы:

- snapshots L2: `timestamp, bid_price_1, bid_size_1, ask_price_1, ask_size_1, ...`;
- incremental updates: `timestamp, side, price, size, action`;
- trades: `timestamp, price, size, aggressor_side`;
- mixed feed: snapshots + updates + trades.

Выходной формат событий:

```python
@dataclass(frozen=True)
class MarketEvent:
    ts: int
    seq: int
    type: Literal["snapshot", "depth_update", "trade"]
    payload: dict
```

Важно:

- сортировка по `timestamp` и `sequence`, если sequence есть;
- нормализация времени в наносекунды или микросекунды;
- проверка дубликатов;
- восстановление книги после snapshot;
- конфигурируемое округление цены к tick size.

#### 4.3.2. LOBBuilder

Задача: поддерживать публичную лимитную книгу на основе исторических событий.

Для MVP достаточно агрегированной Market-By-Price книги:

```python
class OrderBook:
    bids: SortedDict[price, size]  # descending best bid
    asks: SortedDict[price, size]  # ascending best ask

    def apply_snapshot(self, levels): ...
    def apply_update(self, side, price, size): ...
    def best_bid(self) -> tuple[price, size]: ...
    def best_ask(self) -> tuple[price, size]: ...
    def mid(self) -> float: ...
    def spread(self) -> float: ...
```

Требования к книге:

- удалять уровень при `size == 0`;
- проверять `best_bid < best_ask`; если книга crossed/locked, логировать и применять политику восстановления;
- хранить только нужную глубину `max_depth`, если полный depth не требуется;
- отдавать features: spread, mid, imbalance, weighted mid, microprice proxy.

#### 4.3.3. Strategy interface

Стратегия должна быть полностью отделена от движка:

```python
class Strategy(Protocol):
    def on_market_event(self, event: MarketEvent, state: MarketState) -> list[OrderIntent]: ...
    def on_fill(self, fill: FillEvent, state: MarketState) -> list[OrderIntent]: ...
```

`OrderIntent` — не финальный приказ, а намерение стратегии. OrderManager преобразует его в конкретные ордера с учетом risk limits, tick size, min size, cancel policy.

#### 4.3.4. OrderManager / OMS

Задача: управлять собственными ордерами.

Состояния ордера:

```text
CREATED -> SUBMITTED -> ACCEPTED -> PARTIALLY_FILLED -> FILLED
                                \-> CANCEL_REQUESTED -> CANCELLED
                                \-> EXPIRED
                                \-> REJECTED
```

Для MVP можно упростить:

- `ACTIVE`;
- `FILLED`;
- `CANCELLED`;
- `REJECTED`.

Поля собственного ордера:

```python
@dataclass
class Order:
    order_id: str
    ts_submit: int
    side: Literal["buy", "sell"]
    price: float
    qty: float
    remaining_qty: float
    status: OrderStatus
    strategy_id: str
    time_in_force: Literal["GTC", "GTT", "IOC"] = "GTC"
```

Для стратегии market making удобно поддержать cancel/replace:

- на каждом quote refresh отменять старые bid/ask;
- выставлять новые bid/ask вокруг reservation/microprice;
- не выставлять новые ордера, если spread слишком узкий или inventory limit превышен.

#### 4.3.5. ExecutionSimulator / FillModel

Задача: определить, когда собственный ордер исполняется.

MVP-fill model:

```python
def should_fill(order, market_state, event, config) -> bool:
    ref_price = get_reference_price(event, market_state, config.fill_reference)
    if order.side == "buy":
        return ref_price <= order.price
    else:
        return ref_price >= order.price
```

Цена исполнения:

- консервативно: `order.price`;
- альтернативно: `min(order.price, best_ask)` для buy и `max(order.price, best_bid)` для sell;
- для простоты и прозрачности в MVP: **исполнять по цене лимитного ордера**.

Объем исполнения:

- MVP без partial fills: весь `remaining_qty`;
- если добавлять partials: `min(order.remaining_qty, available_qty_at_crossed_level)` или модель по trade size.

Комиссии:

```yaml
fees:
  maker_bps: -0.01    # rebate optional
  taker_bps: 0.05
  default_role: maker
```

Так как стратегия выставляет лимитные ордера, fills считаются maker fills, если ордер не пересек book при выставлении.

#### 4.3.6. Portfolio / Accounting

Задача: учет cash, inventory, realized/unrealized/net PnL.

Для spot-инструмента:

- Buy fill: `cash -= price * qty + fee`, `position += qty`.
- Sell fill: `cash += price * qty - fee`, `position -= qty`.
- Mark-to-market equity: `equity = cash + position * mark_price`.
- PnL: `equity - initial_cash`.

Mark price:

- default: mid-price;
- для ликвидационной оценки можно использовать bid для long и ask для short;
- для microprice-report можно дополнительно считать mark по microprice, но основной PnL лучше фиксировать по mid или conservative liquidation price.

#### 4.3.7. MetricsEngine

Базовые метрики:

```text
final_pnl
net_pnl
gross_pnl
realized_pnl
unrealized_pnl
mean_inventory
max_abs_inventory
inventory_std
turnover_qty
turnover_notional
fill_count
fill_rate
cancel_count
order_count
maker_fee_paid_or_rebate
max_drawdown
sharpe_like_ratio_per_event_or_time_bucket
```

Для market making также полезны:

- average spread captured;
- average quoted spread;
- adverse selection after fill: `mid_{t+h} - fill_price` для buy и `fill_price - mid_{t+h}` для sell;
- inventory skew distribution;
- quote uptime;
- time in market;
- PnL decomposition: spread capture, inventory mark-to-market, fees.

#### 4.3.8. ReportGenerator

На выходе формировать:

- `summary.md`;
- `metrics.csv`;
- `equity_curve.csv`;
- `fills.csv`;
- `orders.csv`;
- графики `equity`, `inventory`, `spread`, `quote_distance`, `fill_price vs mid`.

---

## 5. Детальная event model

### 5.1. Типы событий

```python
class EventType(Enum):
    MARKET_SNAPSHOT = "market_snapshot"
    MARKET_DEPTH_UPDATE = "market_depth_update"
    MARKET_TRADE = "market_trade"
    TIMER = "timer"
    ORDER_SUBMIT = "order_submit"
    ORDER_CANCEL = "order_cancel"
    ORDER_ACCEPTED = "order_accepted"
    ORDER_FILLED = "order_filled"
    ORDER_CANCELLED = "order_cancelled"
```

### 5.2. Основной event loop

```python
for event in data_stream:
    # 1. Обновляем публичную книгу
    market_state = lob_builder.apply(event)

    # 2. Проверяем исполнения уже активных ордеров
    fills = execution_simulator.check_fills(event, market_state, order_manager.active_orders)
    for fill in fills:
        order_manager.apply_fill(fill)
        portfolio.apply_fill(fill)
        strategy.on_fill(fill, market_state)

    # 3. По расписанию или на каждый market event вызываем стратегию
    if scheduler.should_call_strategy(event.ts):
        intents = strategy.on_market_event(event, market_state)
        accepted_orders, cancels = order_manager.process_intents(intents, market_state, risk_manager)

    # 4. Сохраняем снимок метрик
    metrics.update(event.ts, market_state, portfolio, order_manager)
```

### 5.3. Важный порядок обработки

Для честности backtest нужно явно выбрать порядок:

1. Сначала применить market event к книге.
2. Проверить, исполнились ли ранее выставленные ордера на этом событии.
3. Затем вызвать стратегию, чтобы новые ордера не могли исполниться “задним числом” на том же событии.

Если нужно моделировать latency:

- order generated at `ts` becomes active at `ts + latency_submit`;
- cancel generated at `ts` becomes effective at `ts + latency_cancel`;
- если fill случился до прихода cancel на биржу, ордер считается исполненным.

---

## 6. Модель Avellaneda–Stoikov (2008)

### 6.1. Идея модели

Avellaneda–Stoikov — классическая модель optimal market making. Маркет-мейкер выставляет bid и ask лимитные заявки, зарабатывая spread, но несет inventory risk: если он накопил большую позицию, изменение mid-price может привести к убытку. Модель решает задачу максимизации ожидаемой экспоненциальной полезности конечного wealth.

### 6.2. Основные предположения

1. Mid-price следует арифметическому броуновскому движению:

   $$dS_t = \sigma dW_t$$

2. Inventory `q_t` меняется при исполнении bid/ask ордеров.
3. Агрессивные рыночные ордера приходят по пуассоновским процессам.
4. Интенсивность исполнения зависит от расстояния quote от mid/reservation price:

   $$\lambda(\delta) = A e^{-k \delta}$$

   где:

   - `A` — базовая интенсивность потока;
   - `k` — чувствительность интенсивности к удаленности quote;
   - `δ` — расстояние quote от reference price.

5. Маркет-мейкер имеет CARA utility:

   $$U(x) = -e^{-\gamma x}$$

   где `γ` — risk aversion.

6. Целевая функция:

   $$\max E[-e^{-\gamma(X_T + q_T S_T)}]$$

   где `X_T` — cash, `q_T S_T` — стоимость inventory в конце горизонта.

### 6.3. Reservation / indifference price

Reservation price — это “личная fair price” маркет-мейкера с учетом текущего inventory:

$$r_t = S_t - q_t \gamma \sigma^2 (T - t)$$

Интуиция:

- если `q_t > 0`, маркет-мейкер long и хочет продать, поэтому reservation price снижается;
- если `q_t < 0`, он short и хочет купить, поэтому reservation price повышается;
- чем выше `γ`, `σ²` и оставшийся горизонт `(T-t)`, тем сильнее inventory skew.

### 6.4. Optimal spread

В распространенной конечногоризонтной форме оптимальный полный spread:

$$\psi_t = \gamma \sigma^2 (T-t) + \frac{2}{\gamma}\ln\left(1 + \frac{\gamma}{k}\right)$$

Half-spread:

$$h_t = \frac{\psi_t}{2}$$

Quotes:

$$bid_t = r_t - h_t$$

$$ask_t = r_t + h_t$$

Практические ограничения:

- округлить цены к tick size;
- не ставить bid выше/equal ask;
- не пересекать рынок, если стратегия должна быть strictly maker;
- при слишком узком spread пропустить quote или расширить до min spread;
- ограничить inventory через `max_inventory`.

### 6.5. Оценка параметров

#### Волатильность `σ`

Оценить по mid-price returns за rolling window:

```python
returns = mid.diff()
sigma = returns.rolling(window).std() / sqrt(dt)
```

Для price process в A–S часто используется абсолютная price volatility, а не log-return volatility. В документации нужно зафиксировать единицы измерения.

#### Risk aversion `γ`

Это hyperparameter. Подбирается grid search:

```yaml
strategy:
  gamma_grid: [0.001, 0.005, 0.01, 0.05, 0.1]
```

Чем больше `γ`, тем сильнее inventory control и шире quotes.

#### `k` и `A`

`k` можно оценить через зависимость fill intensity от distance-to-mid:

1. Для каждого момента времени и расстояния `δ` от mid оценить, случилось ли пересечение/сделка.
2. Построить эмпирическую интенсивность `λ(δ)`.
3. Fit log-linear model:

   $$\log \lambda(\delta) = \log A - k\delta$$

Для MVP можно задать `k` вручную и провести sensitivity analysis.

### 6.6. Псевдокод стратегии

```python
class AvellanedaStoikovStrategy:
    def on_market_event(self, event, state):
        mid = state.mid
        sigma = self.vol_estimator.update(mid, event.ts)
        tau = max(self.T - self.clock.time_fraction(event.ts), self.min_tau)
        q = self.portfolio.position

        reservation = mid - q * self.gamma * sigma**2 * tau
        total_spread = self.gamma * sigma**2 * tau + (2 / self.gamma) * log(1 + self.gamma / self.k)
        half_spread = total_spread / 2

        bid = round_down_to_tick(reservation - half_spread)
        ask = round_up_to_tick(reservation + half_spread)

        bid, ask = self.apply_market_making_guards(bid, ask, state)

        return [
            CancelAllIntent(strategy_id=self.id),
            SubmitLimitIntent(side="buy", price=bid, qty=self.order_qty),
            SubmitLimitIntent(side="sell", price=ask, qty=self.order_qty),
        ]
```

---

## 7. Microprice и расширение Avellaneda–Stoikov

### 7.1. Что такое microprice

В работе Stoikov (2018) microprice определяется как high-frequency оценка будущей цены, условная на информации из лимитной книги. В статье microprice трактуется как предел последовательности ожидаемых mid-prices; он может быть представлен как корректировка mid-price с учетом spread и imbalance. Автор эмпирически показывает, что microprice лучше предсказывает краткосрочные цены, чем mid-price или weighted mid-price.

### 7.2. Простые order book признаки

Best bid/ask:

$$b_t, a_t$$

Best sizes:

$$V^b_t, V^a_t$$

Mid-price:

$$m_t = \frac{a_t + b_t}{2}$$

Spread:

$$s_t = a_t - b_t$$

Queue imbalance:

$$I_t = \frac{V^b_t - V^a_t}{V^b_t + V^a_t}$$

Weighted mid-price:

$$wm_t = \frac{a_t V^b_t + b_t V^a_t}{V^b_t + V^a_t}$$

Интуиция: если bid size больше ask size, давление спроса выше, и fair price сдвигается ближе к ask.

### 7.3. Практическая microprice proxy

Для MVP можно использовать proxy:

$$mp_t = m_t + \alpha \cdot \frac{s_t}{2} \cdot I_t$$

где `α` — коэффициент от 0 до 1 или обучаемый параметр.

При `α=1` proxy близка к weighted mid для top-of-book imbalance.

Более продвинутый вариант:

1. Дискретизировать imbalance bins: например `[-1,-0.6), ..., (0.6,1]`.
2. Дискретизировать spread в ticks.
3. Оценить условные вероятности следующего движения mid:

   $$P(\Delta m_{t+h} = +tick | I_t, spread_t)$$

   $$P(\Delta m_{t+h} = -tick | I_t, spread_t)$$

4. Определить microprice:

   $$mp_t = m_t + tick \cdot (P_{up} - P_{down})$$

5. Для multi-step extension использовать итеративное ожидание будущего mid-price.

### 7.4. Как совместить microprice с A–S

Есть три практических способа.

#### Вариант A: заменить mid на microprice

В формуле reservation price:

$$r_t = mp_t - q_t \gamma \sigma^2 (T-t)$$

Quotes:

$$bid_t = r_t - h_t$$

$$ask_t = r_t + h_t$$

Это самый простой и понятный вариант.

#### Вариант B: добавить predictive skew

Считать:

$$\phi_t = mp_t - m_t$$

И скорректировать reservation price:

$$r_t = m_t + \beta\phi_t - q_t\gamma\sigma^2(T-t)$$

где `β` подбирается на валидации. Это снижает риск переобучения и позволяет контролировать силу microprice-сигнала.

#### Вариант C: добавить drift term

Если microprice интерпретируется как прогноз краткосрочного drift:

$$\mu_t \approx \frac{mp_t - m_t}{h}$$

Тогда reservation price можно записать как:

$$r_t = m_t + \mu_t (T-t) - q_t\gamma\sigma^2(T-t)$$

Этот вариант требует аккуратной калибровки горизонта `h`, иначе drift term может стать слишком большим.

### 7.5. Рекомендуемый вариант для задания

Для вступительного задания лучше реализовать оба режима:

```yaml
strategy:
  name: microprice_avellaneda_stoikov
  fair_price_mode: microprice_proxy   # mid | weighted_mid | microprice_proxy | learned_microprice
  microprice_beta: 0.5
```

Основная формула:

$$r_t = m_t + \beta(mp_t - m_t) - q_t\gamma\sigma^2(T-t)$$

Это дает понятный контроль:

- `β=0` — обычный A–S;
- `β=1` — полное использование microprice;
- `0<β<1` — сглаженная версия.

---

## 8. Конфигурация экспериментов

### 8.1. Пример YAML-конфига

```yaml
run:
  seed: 42
  symbol: BTCUSDT
  start_time: "2024-01-01T00:00:00Z"
  end_time: "2024-01-01T01:00:00Z"
  output_dir: "reports/run_001"

data:
  path: "data/raw/MD.zip"
  format: "auto"
  timestamp_unit: "ns"
  max_depth: 20
  tick_size: 0.01
  lot_size: 0.001

engine:
  event_mode: "event_driven"
  strategy_call: "timer"
  quote_refresh_ms: 100
  latency_submit_ms: 0
  latency_cancel_ms: 0

execution:
  fill_model: "price_cross"
  fill_reference: "trade_price"
  fallback_reference: "best_quote"
  fill_price: "limit_price"
  partial_fills: false
  queue_model: "none"

fees:
  maker_bps: 0.0
  taker_bps: 0.0

portfolio:
  initial_cash: 100000.0
  max_inventory: 1.0
  mark_price: "mid"

strategy:
  name: "microprice_avellaneda_stoikov"
  order_qty: 0.01
  gamma: 0.01
  k: 1.5
  sigma_window_ms: 60000
  horizon_seconds: 300
  min_spread_ticks: 1
  fair_price_mode: "microprice_proxy"
  microprice_beta: 0.5
  cancel_replace: true

report:
  save_orders: true
  save_fills: true
  save_equity_curve: true
  plots: true
```

### 8.2. Baseline strategies

Чтобы отчет выглядел убедительно, сравнить минимум три режима:

1. **Naive symmetric market maker**: always quote best bid/best ask или mid ± fixed spread.
2. **Avellaneda–Stoikov**: inventory-aware quoting.
3. **Microprice A–S**: inventory-aware + order book imbalance/fair price shift.

Дополнительно:

4. **No trading** baseline: PnL = 0, используется для sanity check.
5. **Random quoting** baseline: контроль переобучения.

### 8.3. Sensitivity analysis

Grid:

```yaml
grid:
  gamma: [0.001, 0.005, 0.01, 0.05]
  k: [0.5, 1.0, 1.5, 2.0]
  microprice_beta: [0.0, 0.25, 0.5, 0.75, 1.0]
  order_qty: [0.005, 0.01, 0.02]
  quote_refresh_ms: [50, 100, 250, 500]
```

Для каждого run сохранить:

- итоговый PnL;
- max drawdown;
- mean/max inventory;
- turnover;
- fill rate;
- fees;
- realized/unrealized PnL;
- adverse selection metric.

### 8.4. Walk-forward split

Если данных достаточно:

- train/calibration: 60%;
- validation: 20%;
- test: 20%.

На train оцениваются `sigma`, `k`, microprice bins/transition probabilities. На validation подбираются `gamma`, `beta`, order size, quote refresh. На test фиксируются параметры и считается финальная производительность.

---

## 9. Метрики и формулы

### 9.1. Accounting

Для каждого fill:

```python
notional = fill.price * fill.qty
fee = abs(notional) * fee_bps / 10000

if fill.side == "buy":
    cash -= notional + fee
    position += fill.qty
else:
    cash += notional - fee
    position -= fill.qty
```

Equity:

```python
equity_t = cash_t + position_t * mark_price_t
pnl_t = equity_t - initial_cash
```

### 9.2. Turnover

```python
turnover_qty = sum(abs(fill.qty) for fill in fills)
turnover_notional = sum(abs(fill.qty * fill.price) for fill in fills)
turnover_ratio = turnover_notional / initial_cash
```

### 9.3. Inventory risk

```python
mean_inventory = mean(position_t)
max_abs_inventory = max(abs(position_t))
inventory_std = std(position_t)
time_at_limit = mean(abs(position_t) >= max_inventory)
```

### 9.4. Fill metrics

```python
fill_rate = filled_orders / submitted_orders
cancel_rate = cancelled_orders / submitted_orders
avg_time_to_fill = mean(fill_ts - submit_ts)
```

### 9.5. Spread capture

Для buy:

```python
spread_capture = mid_at_fill - fill_price
```

Для sell:

```python
spread_capture = fill_price - mid_at_fill
```

### 9.6. Adverse selection

Через горизонт `h`:

Для buy:

```python
adverse_selection_h = mid_{t+h} - fill_price
```

Для sell:

```python
adverse_selection_h = fill_price - mid_{t+h}
```

Если значение отрицательное, сделка была неблагоприятной относительно будущего mid.

---

## 10. План реализации по этапам

### Этап 0. Подготовка репозитория

**Цель:** создать структуру проекта, окружение, линтеры, тесты, CLI.

Deliverables:

- `pyproject.toml`;
- `README.md`;
- базовая структура `src/`, `tests/`, `configs/`, `reports/`;
- `pytest`, `ruff`, `mypy` или `pyright`;
- CLI `python -m lob_backtester run --config configs/as.yaml`.

### Этап 1. Data audit и нормализация

**Цель:** понять формат `MD.zip`, написать загрузчик.

Задачи:

- распаковать архив;
- определить типы файлов: CSV, parquet, json, npy;
- определить колонки и единицы timestamp;
- написать schema inference;
- создать нормализованный event stream;
- добавить sample dataset в `data/sample/`.

Acceptance criteria:

- `DataLoader` читает sample data;
- события отсортированы;
- есть тест на порядок timestamp;
- можно ограничивать диапазон времени.

### Этап 2. LOB replay

**Цель:** восстановить книгу и top-of-book.

Задачи:

- реализовать `OrderBook`;
- применить snapshots/updates;
- рассчитать best bid/ask/mid/spread/imbalance;
- логировать crossed/locked book;
- сохранить `book_features.csv` для sanity checks.

Acceptance criteria:

- после каждого события книга валидна;
- best bid < best ask;
- mid/spread считаются корректно;
- тесты покрывают snapshot, update, delete level.

### Этап 3. OrderManager и FillSimulator

**Цель:** поддержать submit/cancel/fill собственных лимитных ордеров.

Задачи:

- dataclasses для Order, Fill, OrderIntent;
- active order store;
- cancel/replace;
- fill by price crossing;
- fee model;
- orders/fills logs.

Acceptance criteria:

- buy limit исполняется при price <= limit;
- sell limit исполняется при price >= limit;
- cancel удаляет active order;
- после fill обновляется status;
- тесты на buy/sell/cancel/fill.

### Этап 4. Portfolio и Metrics

**Цель:** учет капитала и расчет метрик.

Задачи:

- accounting для buy/sell;
- mark-to-market equity curve;
- PnL, inventory, turnover;
- summary report.

Acceptance criteria:

- PnL сходится на ручном сценарии;
- cash/position не ломаются;
- есть `metrics.json`, `equity_curve.csv`.

### Этап 5. Strategy interface и naive baseline

**Цель:** проверить интеграцию стратегии с движком.

Задачи:

- интерфейс `Strategy`;
- стратегия fixed spread around mid;
- quote refresh scheduler;
- risk guard: max inventory.

Acceptance criteria:

- baseline генерирует bid/ask;
- старые quote отменяются;
- max inventory не нарушается;
- есть отчет по baseline.

### Этап 6. Avellaneda–Stoikov

**Цель:** реализовать A–S модель.

Задачи:

- rolling volatility estimator;
- параметры `gamma`, `k`, `horizon`;
- reservation price;
- optimal spread;
- tick rounding;
- inventory skew;
- конфиг и отчет.

Acceptance criteria:

- при positive inventory quotes сдвигаются вниз;
- при negative inventory quotes сдвигаются вверх;
- spread растет при росте `gamma` и `sigma`;
- тесты на формулы.

### Этап 7. Microprice extension

**Цель:** добавить microprice/imbalance-aware fair price.

Задачи:

- top-of-book imbalance;
- weighted mid;
- microprice proxy;
- `beta` для skew strength;
- optional learned microprice через bins.

Acceptance criteria:

- при bid imbalance fair price выше mid;
- при ask imbalance fair price ниже mid;
- `beta=0` эквивалентен обычному A–S;
- `beta=1` полностью использует microprice proxy.

### Этап 8. Эксперименты и отчет

**Цель:** сравнить стратегии и показать результаты.

Задачи:

- запустить baseline/A–S/Microprice-A–S;
- grid search по ключевым параметрам;
- сформировать таблицу метрик;
- построить графики;
- описать выводы и ограничения.

Acceptance criteria:

- есть `report.md`;
- есть таблица сравнения;
- есть графики equity/inventory;
- есть выводы по устойчивости параметров.

---

## 11. Структура репозитория

```text
lob-backtester/
├── README.md
├── pyproject.toml
├── configs/
│   ├── baseline_fixed_spread.yaml
│   ├── avellaneda_stoikov.yaml
│   └── microprice_avellaneda_stoikov.yaml
├── data/
│   ├── raw/                 # не коммитить большие файлы
│   └── sample/              # маленький пример для тестов/демо
├── reports/
│   └── .gitkeep
├── src/
│   └── lob_backtester/
│       ├── __init__.py
│       ├── cli.py
│       ├── config.py
│       ├── data/
│       │   ├── loader.py
│       │   ├── schema.py
│       │   └── events.py
│       ├── book/
│       │   ├── order_book.py
│       │   └── features.py
│       ├── engine/
│       │   ├── backtest.py
│       │   ├── scheduler.py
│       │   └── event_loop.py
│       ├── execution/
│       │   ├── order.py
│       │   ├── order_manager.py
│       │   ├── fill_model.py
│       │   └── fees.py
│       ├── portfolio/
│       │   ├── accounting.py
│       │   └── risk.py
│       ├── strategies/
│       │   ├── base.py
│       │   ├── fixed_spread.py
│       │   ├── avellaneda_stoikov.py
│       │   └── microprice_as.py
│       ├── metrics/
│       │   ├── metrics.py
│       │   └── report.py
│       └── utils/
│           ├── rounding.py
│           └── time.py
└── tests/
    ├── test_order_book.py
    ├── test_fill_model.py
    ├── test_accounting.py
    ├── test_avellaneda_stoikov.py
    └── test_microprice.py
```

---

## 12. Python vs C++

| Критерий | Python | C++ |
|---|---|---|
| Скорость разработки | Очень высокая | Ниже |
| Аналитика и графики | Отлично: pandas, numpy, matplotlib | Требует дополнительных инструментов |
| Производительность | Достаточно для MVP; ускоряется numba/polars | Максимальная |
| Тестируемость | Простая | Хорошая, но больше boilerplate |
| Работа с parquet/csv | Удобная | Возможна, но сложнее |
| Демонстрация модели | Отлично | Менее удобно |
| HFT-like replay больших данных | Может быть bottleneck | Сильная сторона |

Рекомендация: **реализовать на Python**, но сделать чистые интерфейсы и минимизировать pandas внутри event loop. Внутри цикла использовать dataclasses/arrays и простые структуры данных. Если производительность станет проблемой:

1. заменить pandas на polars/pyarrow для загрузки;
2. вынести LOB update в `numba`;
3. переписать matching/fill simulation на C++/Rust;
4. оставить Python для orchestration/reporting.

---

## 13. Тестирование

### 13.1. Unit tests

- `OrderBook.apply_snapshot`;
- `OrderBook.apply_update`;
- удаление price level;
- best bid/ask after updates;
- fill rules for buy/sell;
- cancel semantics;
- accounting for buy/sell;
- A–S formulas;
- microprice proxy formula.

### 13.2. Integration tests

Синтетический сценарий:

```text
t=0: best bid=99, best ask=101
strategy submits buy 99, sell 101
 t=1: trade price=99 -> buy filled
 t=2: mid moves to 100
 t=3: trade price=101 -> sell filled
```

Проверить:

- inventory вернулся к 0;
- PnL соответствует spread capture minus fees;
- order statuses корректны.

### 13.3. Sanity checks

- no trading strategy дает PnL=0;
- если fees=0 и market не двигается, symmetric market maker должен зарабатывать spread при round-trip fills;
- при high gamma inventory должен быть ниже, чем при low gamma;
- при `beta=0` microprice strategy совпадает с A–S.

---

## 14. Roadmap улучшений

### 14.1. После MVP

1. **Partial fills**: исполнять часть ордера по доступному объему или trade size.
2. **Queue position model**: оценивать позицию в очереди по Market-By-Price данным.
3. **Latency model**: submit/cancel latency, jitter, exchange ack delay.
4. **Market impact**: хотя бы простой impact для больших ордеров.
5. **Fees/rebates**: maker/taker, tiered fees.
6. **Multi-level microprice**: imbalance по нескольким уровням книги.
7. **Learned microprice**: transition matrix или ML-модель one-tick-ahead move.
8. **Walk-forward optimization**: train/validation/test.
9. **PnL decomposition**: spread capture, inventory drift, adverse selection, fees.
10. **Performance optimization**: numba/C++ core.

### 14.2. Более продвинутые модели

- Guéant–Lehalle–Fernandez-Tapia style market making;
- skew by alpha signal;
- dynamic order size;
- reinforcement learning policy для quote placement;
- multi-asset inventory constraints;
- stress testing на regime shifts.

---

## 15. References

1. Marco Avellaneda and Sasha Stoikov, “High-frequency trading in a limit order book,” Quantitative Finance, 2008, 8(3), 217–224. DOI: `10.1080/14697680701381228`.
   https://www.tandfonline.com/doi/full/10.1080/14697680701381228

2. NYU-hosted PDF of Avellaneda–Stoikov paper.
   https://math.nyu.edu/inmemoriam/avellaneda/HighFrequencyTrading.pdf

3. Sasha Stoikov, “The micro-price: a high-frequency estimator of future prices,” Quantitative Finance, 2018, 18(12), 1959–1966. DOI: `10.1080/14697688.2018.1489139`.
   https://www.tandfonline.com/doi/full/10.1080/14697688.2018.1489139

4. SSRN version: Sasha Stoikov, “The Micro-Price: A High Frequency Estimator of Future Prices.”
   https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2970694

5. QuantStart, “Event-Driven Backtesting with Python — Part I.”
   https://www.quantstart.com/articles/Event-Driven-Backtesting-with-Python-Part-I/

6. HftBacktest documentation, “Order Fill.”
   https://hftbacktest.readthedocs.io/en/latest/order_fill.html

7. Backtrader documentation, “Orders.”
   https://www.backtrader.com/docu/order/

8. Data link from task, resolved through LinkedIn to Google Drive file `MD.zip`.
   https://drive.google.com/file/d/1DiP5arvCEMxLHZ0R2mAS4lcMjnPHSrEJ

9. Application form link from task, resolved through LinkedIn to Google Forms.
   https://forms.gle/oLkbttqddGxnkWzr9

---

## 16. Итоговый checklist для сдачи

- [ ] `README.md` с описанием запуска.
- [ ] `configs/*.yaml` для baseline, A–S, Microprice-A–S.
- [ ] `src/` с backtest engine, LOB, OMS, fill model, strategies.
- [ ] `tests/` с unit/integration тестами.
- [ ] `data/sample/` с небольшим примером данных.
- [ ] `reports/report.md` с результатами.
- [ ] `orders.csv`, `fills.csv`, `equity_curve.csv`, `metrics.json`.
- [ ] Графики PnL/inventory/turnover/fill rate.
- [ ] Раздел с ограничениями модели исполнения.
- [ ] Roadmap улучшений.
