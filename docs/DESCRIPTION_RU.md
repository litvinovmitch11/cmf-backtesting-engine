# Полное описание движка бэктестинга и торговых стратегий

> Документ объясняет **как устроен симулятор** и **как работает каждая стратегия**.
> Каждый раздел построен одинаково: сначала — **простыми словами**, потом —
> **математика и доказательство из кода** (со ссылками на конкретные файлы и строки).
>
> Связанные документы: [README.md](../README.md), [ARCHITECTURE.md](ARCHITECTURE.md),
> [STRATEGY.md](STRATEGY.md). Статьи в [docs/papers/](papers/).

---

## Оглавление

1. [Что это вообще такое (простыми словами)](#1-что-это-вообще-такое)
2. [Данные: формат и загрузка](#2-данные-формат-и-загрузка)
3. [Слияние потоков (FeedMerger)](#3-слияние-потоков-feedmerger)
4. [Стакан (OrderBook)](#4-стакан-orderbook)
5. [Движок и главный цикл (BacktestEngine)](#5-движок-и-главный-цикл-backtestengine)
6. [Симуляция исполнения (ExecutionSimulator)](#6-симуляция-исполнения-executionsimulator)
7. [Очередь и латентность](#7-очередь-и-латентность)
8. [Метрики: PnL, инвентарь, оборот](#8-метрики-pnl-инвентарь-оборот)
9. [Временной ряд и графики](#9-временной-ряд-и-графики)
10. [Стратегия 1 — FixedSpreadQuoter (база)](#10-стратегия-1--fixedspreadquoter-база)
11. [Стратегия 2 — Avellaneda–Stoikov 2008 (каноничная)](#11-стратегия-2--avellanedastoikov-2008)
12. [Стратегия 3 — Micro-price (Stoikov 2018)](#12-стратегия-3--micro-price-stoikov-2018)
13. [Стратегия 4 — Avellaneda–Stoikov Online (рабочая)](#13-стратегия-4--avellanedastoikov-online)
14. [Стратегия 5 — Micro-price + Online A-S (лучшая)](#14-стратегия-5--micro-price--online-a-s-лучшая)
15. [Калибровка параметров](#15-калибровка-параметров)
16. [Результаты на полных данных](#16-результаты-на-полных-данных)
17. [Допущения, ограничения, тесты](#17-допущения-ограничения-тесты)

---

## 1. Что это вообще такое

**Простыми словами.** Это «машина времени» для торговых стратегий. У нас есть
записанная история рынка: как менялся биржевой стакан (кто и почём готов купить/продать)
и какие происходили сделки. Движок проигрывает эту историю событие за событием, как
видеозапись. В это время наша стратегия — маркет-мейкер — выставляет свои лимитные
заявки (купить чуть дешевле, продать чуть дороже). Движок честно моделирует: встала ли
наша заявка в очередь, когда она исполнилась, по какой цене. После прогона мы считаем
итог: сколько заработали (**PnL**), какая осталась позиция (**инвентарь**), какой был
оборот (**turnover**).

**Ключевое допущение** — наша торговля *не влияет на рынок* (модель «price-taker
overlay»). Мы как бы накладываем свои заявки поверх записанной ленты: лента развивается
ровно так, как было исторически, а мы лишь смотрим, что бы исполнилось из наших заявок.
Это делает абсолютный PnL оптимистичным, поэтому ценность — в **сравнении стратегий
между собой**, а не в абсолютной цифре.

**Из чего состоит система (поток данных):**

```
CSV (стакан, сделки)
   └─ convert_csv ─► packed .bin (фикс. размер записей, mmap)
                        │
        LobBinSource ───┤
                        ├─► FeedMerger (мин-куча по времени) ─► BacktestEngine
      TradesBinSource ──┘                                          │
                                                                   ├─ OrderBook (стакан)
                                                                   ├─ Strategy (логика)
                                                                   ├─ ExecutionSimulator (исполнение)
                                                                   └─ Metrics (PnL / ряд)
```

Язык — современный **C++23**. Всё однопоточное и **детерминированное**: два прогона
дают побитово одинаковый отчёт.

---

## 2. Данные: формат и загрузка

**Простыми словами.** Исходные данные приходят в CSV (текст), но текст читать медленно.
Поэтому есть отдельная утилита, которая один раз переводит CSV в плотный бинарный формат
с записями фиксированного размера. Потом движок не «читает файл» в обычном смысле, а
**отображает его в память** (`mmap`) и работает с записями прямо там, без копирования —
отсюда скорость ~25 млн событий/сек.

**Цены — целые числа («тики»).** Чтобы никогда не сравнивать дробные цены на равенство
(это опасно из-за погрешностей `double`), цена хранится как целое число тиков:
`price_in_ticks = round(price * 10_000_000)`. Масштаб `kPriceScale = 1e7` (7 знаков
после запятой).

Доказательство — [core/types.hpp](../include/bt/core/types.hpp):

```cpp
using Ticks = std::int64_t;
inline constexpr std::int64_t kPriceScale = 10'000'000;     // тик = 1e-7

Ticks  to_ticks(double price) { return llround(price * kPriceScale); }  // цена -> тики
double to_price(Ticks t)      { return double(t) / kPriceScale; }       // тики -> цена
```

**Бинарный формат** — [data/binary_record.hpp](../include/bt/data/binary_record.hpp).
Каждый файл начинается с заголовка `BinHeader` (40 байт: «магия» для проверки типа файла,
версия, глубина стакана, масштаб цены, число записей). Затем идут записи фикс. размера:

```cpp
inline constexpr std::uint32_t kLobDepth = 25;      // 25 уровней с каждой стороны

struct LobRecord {              // 808 байт
  std::int64_t ts;             // время, микросекунды
  BookLevel bids[25];          // {цена в тиках, объём} x 25
  BookLevel asks[25];
};
struct TradeRecord {            // 32 байта
  std::int64_t ts;
  std::int64_t px;             // цена сделки в тиках
  double qty;
  std::uint8_t side;           // 0 = Buy-агрессор, 1 = Sell-агрессор
};
```

**Конвертация CSV → bin** — [data/csv_convert.cpp](../src/bt/data/csv_convert.cpp).
Парсер быстрый и locale-независимый (`std::from_chars`). Для стакана берётся столбец
времени и 25 уровней `(ask_px, ask_qty, bid_px, bid_qty)`; для сделок — время, сторона
(`buy`/`sell`), цена, объём. Каждая дробная цена тут же переводится в тики, а
максимальная ошибка округления отслеживается (`max_tick_round_err`) — контроль того, что
масштаб тика выбран правильно.

**Чтение через mmap** — [data/mmap_file.hpp](../include/bt/data/mmap_file.hpp) (RAII-обёртка
над `open`/`mmap`/`munmap`, с `madvise(MADV_SEQUENTIAL)`), а сами источники событий
возвращают `BookSnapshot`, который **указывает прямо в отображённую память** (ноль
копирования) — [data/lob_source.cpp](../src/bt/data/lob_source.cpp):

```cpp
MarketEvent LobBinSource::next() {
  const LobRecord& r = records_[idx_++];
  return BookUpdate{.ts = r.ts, .book = BookSnapshot{r.bids, r.asks, kLobDepth}};
}                                   // указатели r.bids/r.asks живут в mmap
```

**Событие рынка** — это вариант из двух типов
([core/event.hpp](../include/bt/core/event.hpp)):

```cpp
struct TradePrint { Ts ts; Side aggressor; Ticks price; Qty amount; };
struct BookUpdate { Ts ts; BookSnapshot book; };
using MarketEvent = std::variant<BookUpdate, TradePrint>;
```

---

## 3. Слияние потоков (FeedMerger)

**Простыми словами.** У нас два отдельных потока: обновления стакана и сделки. Их нужно
проиграть в строгом хронологическом порядке, как если бы это была одна общая лента.
`FeedMerger` берёт несколько источников и на каждом шаге выдаёт самое раннее по времени
событие. Реализовано через **двоичную мин-кучу** по времени следующего события каждого
источника.

**Детерминизм.** Если у двух событий одинаковое время, побеждает источник, добавленный
раньше (меньший индекс). Поэтому порядок полностью предопределён, а прогон —
воспроизводим.

Доказательство — [data/feed_merger.cpp](../src/bt/data/feed_merger.cpp):

```cpp
struct HeapCmp {
  bool operator()(const HeapItem& a, const HeapItem& b) const {
    // «больше» => std::*_heap работает как МИН-куча по ts;
    // при равном ts побеждает меньший индекс источника (детерминизм).
    return a.ts != b.ts ? a.ts > b.ts : a.idx > b.idx;
  }
};

MarketEvent FeedMerger::next() {
  std::pop_heap(...);                       // достаём самый ранний
  const HeapItem item = heap_.back(); heap_.pop_back();
  MarketEvent ev = sources_[item.idx]->next();
  if (sources_[item.idx]->has_next())       // возвращаем источник в кучу
    { heap_.push_back({sources_[item.idx]->peek_ts(), item.idx}); std::push_heap(...); }
  return ev;
}
```

Порядок добавления в [apps/backtest.cpp](../apps/backtest.cpp): канал 0 — стакан,
канал 1 — сделки. Значит, при совпадении времени сначала обновится стакан, потом придёт
сделка — это логично (сделка происходит «на» текущем стакане).

---

## 4. Стакан (OrderBook)

**Простыми словами.** Стакан — это текущая картинка рынка: лучшая цена покупки
(`best_bid`), лучшая цена продажи (`best_ask`), объёмы на них и глубже. Наш `OrderBook`
не хранит копию — он держит лёгкую «ссылку» на последний снимок из mmap. Сделки стакан
не меняют (это просто принты ленты); меняют его только события `BookUpdate`.

**Математика (определения).**

```
mid        = (bid + ask) / 2                       — справедливая «середина»
spread     = ask - bid                             — ширина рынка (в тиках)
imbalance  I = Qb / (Qb + Qa)                       — перекос объёмов наверху
microprice = (bid·Qa + ask·Qb) / (Qa + Qb)          — простая объёмо-взвешенная середина
```

Заметьте: в `microprice()` цена бида умножается на объём **аска** и наоборот — поэтому
когда биды «тяжелее» (`Qb` большой), оценка тянется к аску (цена скорее пойдёт вверх).

Доказательство — [book/order_book.hpp](../include/bt/book/order_book.hpp):

```cpp
double mid()    const { return 0.5 * (to_price(best_bid()) + to_price(best_ask())); }
Ticks  spread() const { return best_ask() - best_bid(); }

double microprice() const {                 // простой объёмо-взвешенный mid
  double qb = best_bid_qty(), qa = best_ask_qty(), denom = qb + qa;
  if (denom <= 0) return mid();
  return (to_price(best_bid())*qa + to_price(best_ask())*qb) / denom;
}

Qty size_at(Side side, Ticks px) const {    // объём на конкретном уровне (для очереди)
  const BookLevel* lv = (side == Side::Buy) ? snap_.bids : snap_.asks;
  for (uint32_t i = 0; i < snap_.depth; ++i) if (lv[i].px == px) return lv[i].qty;
  return 0.0;
}
```

> Важно: встроенный `microprice()` в стакане — это **упрощённая** объёмо-взвешенная
> середина. Полноценный микропрайс Стойкова (2018) считается отдельной марковской
> моделью (раздел 12), а не этой формулой.

---

## 5. Движок и главный цикл (BacktestEngine)

**Простыми словами.** Движок — это «дирижёр». Он крутит цикл: взять следующее событие →
обновить мир → дать стратегии отреагировать → проверить исполнения → обновить метрики.
Он же является «руками» стратегии: когда стратегия говорит «поставь заявку», вызов идёт
в движок, а он передаёт его симулятору исполнения, проставляя текущее время.

**Точный порядок действий на каждое событие** (это критично для корректности) —
[engine/backtest_engine.cpp](../src/bt/engine/backtest_engine.cpp):

```cpp
while (feed_.has_next()) {
  MarketEvent ev = feed_.next();  now_ = timestamp_of(ev);    // 1. взять событие, выставить часы

  if (BookUpdate* bu = ...) {
    book_.apply(*bu);                    // 2а. обновить стакан
    exec_.on_book_update();              // 2б. пересчитать оценки очереди
    strat_.on_book(book_, now_, *this);  // 2в. стратегия реагирует (ставит/снимает заявки)
  } else if (TradePrint* tp = ...) {
    exec_.on_trade(*tp);                 // 2а'. пассивные исполнения от этого принта
    strat_.on_trade(*tp, book_, now_, *this);
  }

  exec_.activate_due(now_);             // 3. активировать «долетевшие» заявки (+ маркетабельные исполнить)
  for (const Fill& f : exec_.take_fills()) {  // 4. разнести исполнения
    strat_.on_fill(f);  metrics_.on_fill(f);  ++st.fills;
  }
  if (book_.valid()) metrics_.on_mark(now_, book_.mid());     // 5. переоценка по mid
}
metrics_.finalize();
```

Почему именно так:
- **Сначала обновляем стакан, потом зовём стратегию** — стратегия видит свежую картину.
- **`activate_due` после реакции стратегии** — заявка, поставленная сейчас, не может
  исполниться мгновенно: ей нужно «долететь» (латентность), см. раздел 7.
- **Mark-to-market по `mid` на каждом событии** — инвентарь постоянно переоценивается,
  поэтому equity-кривая гладкая.

Движок реализует интерфейс `OrderApi`
([strategy/order_api.hpp](../include/bt/strategy/order_api.hpp)) — это всё, что стратегия
знает о внешнем мире для торговли:

```cpp
OrderId BacktestEngine::place(Side side, Ticks px, Qty qty) { return exec_.place(side, px, qty, now_); }
void    BacktestEngine::cancel(OrderId id)                  { exec_.cancel(id); }
```

---

## 6. Симуляция исполнения (ExecutionSimulator)

Это сердце реализма. Здесь решается, **исполнилась ли наша заявка** и по какой цене.

**Простыми словами.** Наши заявки — это «оверлей» поверх записанного рынка; в самой ленте
их нет. Заявка может исполниться двумя способами:

1. **Как тейкер (берём ликвидность).** Если в момент, когда заявка становится активной,
   она уже «перекрывает» рынок (наш бид ≥ лучший аск, или наш аск ≤ лучший бид), она
   сразу съедает встречные уровни стакана. Цена исполнения — цена уровней стакана.
2. **Как мейкер (даём ликвидность).** Заявка спокойно стоит в очереди. Когда по ленте
   проходит сделка, которая «дотягивается» до нашей цены, она сначала проедает объём,
   стоящий **перед нами** в очереди, а остаток исполняет нас. Цена — наша цена заявки.

Это и есть **Execution Assumption** из задания: «исполнение происходит, когда рыночная
цена пересекает уровень заявки».

### 6.1. Жизненный цикл заявки и латентность

Заявка не встаёт в стакан мгновенно — есть задержка `order_latency`. До этого она
«в полёте» (`active = false`). Доказательство —
[exec/execution_simulator.cpp](../src/bt/exec/execution_simulator.cpp):

```cpp
OrderId ExecutionSimulator::place(Side side, Ticks px, Qty qty, Ts now) {
  Order o{}; o.id = next_id_++; o.side = side; o.px = px;
  o.qty = qty; o.remaining = qty; o.placed_ts = now;
  o.active_ts = latency_.order_arrival(now);   // = now + order_latency
  o.active = false;                            // пока «в полёте»
  live_.push_back(o); return o.id;
}
```

`activate_due(now)` промотивирует все заявки, чьё время прилёта наступило:

```cpp
void ExecutionSimulator::activate_due(Ts now) {
  for (Order& o : live_) if (!o.active && o.active_ts <= now) activate(o, now);
  compact();                                   // выбросить полностью исполненные
}
```

### 6.2. Активация: маркетабельность и постановка в очередь

```cpp
void ExecutionSimulator::activate(Order& o, Ts now) {
  o.active = true;
  if (book_.valid()) {
    bool marketable = (o.side==Buy  && o.px >= book_.best_ask()) ||   // наш бид перекрыл аск
                      (o.side==Sell && o.px <= book_.best_bid());     // наш аск перекрыл бид
    if (marketable) fill_marketable(o, now);   // мгновенно берём ликвидность
  }
  if (o.remaining > kEps) {                     // остаток встаёт пассивно
    o.queue_ahead     = queue_.initial_queue(book_, o.side, o.px);  // объём перед нами = текущий объём на уровне
    o.last_level_size = o.queue_ahead;
  }
}
```

**Тейкерское исполнение** проходит по уровням встречной стороны, пока хватает цены и
остатка (частичные исполнения разрешены), `maker = false`:

```cpp
void ExecutionSimulator::fill_marketable(Order& o, Ts now) {
  const BookLevel* levels = (o.side==Buy) ? s.asks : s.bids;
  for (uint32_t i = 0; i < s.depth && o.remaining > kEps; ++i) {
    bool reachable = (o.side==Buy) ? (levels[i].px <= o.px) : (levels[i].px >= o.px);
    if (!reachable) break;                      // уровни упорядочены по цене
    Qty f = std::min(o.remaining, levels[i].qty);
    o.remaining -= f;
    fills_.push_back({.price=levels[i].px, .qty=f, .maker=false, ...});  // цена = цена уровня
  }
}
```

### 6.3. Мейкерское исполнение от сделок (queue-aware)

Главная функция реализма — `on_trade`. Условие пересечения дословно реализует
Execution Assumption:

```cpp
void ExecutionSimulator::on_trade(const TradePrint& trade) {
  // Sell-агрессор бьёт по нашим бидам с ценой >= цены сделки;
  // Buy-агрессор  бьёт по нашим аскам с ценой <= цены сделки.
  for (...) {
    bool crossed = (o.side==Buy  && trade.aggressor==Sell && trade.price <= o.px) ||
                   (o.side==Sell && trade.aggressor==Buy  && trade.price >= o.px);
    if (crossed) elig.push_back(i);
  }
  // Приоритет цена-время среди НАШИХ заявок: лучшая цена раньше, при равенстве — меньший id.
  std::sort(elig, by price then id);

  Qty avail = trade.amount;                     // объём одной сделки делится между нашими заявками
  for (idx : elig) {
    Qty eat = std::min(o.queue_ahead, avail);   // сперва проедается очередь ПЕРЕД нами
    o.queue_ahead -= eat;  avail -= eat;
    if (o.queue_ahead > kEps || avail <= kEps) continue;   // мы ещё не на фронте / объём кончился
    Qty f = std::min(o.remaining, avail);       // остаток исполняет нас (частично можно)
    o.remaining -= f;  avail -= f;
    fills_.push_back({.price=o.px, .qty=f, .maker=true, ...});  // цена = НАША цена
  }
}
```

Ключевые идеи:
- **Очередь перед нами (`queue_ahead`)** проедается первой — мы не исполняемся, пока
  объём сделки не «прошёл» весь объём, стоявший на уровне раньше нас.
- **Один принт делится** между несколькими нашими заявками по приоритету цена-время.
- **Частичные исполнения** поддержаны (`f = min(remaining, avail)`).

---

## 7. Очередь и латентность

**Простыми словами.** Биржа даёт нам только *суммарный* объём на уровне, а не отдельные
заявки. Значит, нашу позицию в очереди мы можем лишь **оценивать**. Для этого есть две
модели поведения при отменах чужих заявок.

Доказательство — [exec/queue_model.hpp](../include/bt/exec/queue_model.hpp):

```cpp
// При входе мы всегда встаём в конец: перед нами — весь текущий объём уровня.
Qty initial_queue(book, side, px) { return book.size_at(side, px); }

// 1) Оптимистичная: отмены считаем «за нами», поэтому только сделки уменьшают очередь.
//    Верхняя оценка числа исполнений.
class OptimisticQueue   { void on_book_change(...) {} };

// 2) Пропорциональная: отмены «размазаны» по очереди, падение видимого объёма
//    уменьшает и нашу позицию. Более консервативно.
class ProportionalQueue {
  void on_book_change(Order& o, Qty old_size, Qty new_size) {
    if (new_size < old_size && old_size > 0) o.queue_ahead *= new_size / old_size;
  }
};
```

При каждом `BookUpdate` движок вызывает `on_book_update()`, который пересчитывает
`queue_ahead` активных заявок согласно выбранной модели
([execution_simulator.cpp](../src/bt/exec/execution_simulator.cpp)).

**Латентность** — [exec/latency_model.hpp](../include/bt/exec/latency_model.hpp): простая
константная модель. `order_latency` (задержка постановки заявки) применяет симулятор;
`feed_latency` (задержка получения данных) пока зарезервирована (в дорожной карте).

```cpp
Ts order_arrival(Ts decision_ts) const { return decision_ts + order_latency; }
```

---

## 8. Метрики: PnL, инвентарь, оборот

**Простыми словами.** Считаем по «бухгалтерии денежного потока». Купили — заплатили деньги
(`cash` уменьшился), позиция выросла. Продали — получили деньги, позиция упала. В любой
момент наш «капитал» = деньги на счёте + текущая позиция, переоценённая по `mid`.

**Математика.** Для исполнения объёма `q` по цене `p` (notional `N = q·p`) и комиссии
`fee_bps` (в десятых долях процента, ×1e-4):

```
Buy :  cash -= N ;  inventory += q
Sell:  cash += N ;  inventory -= q
fee   = N · fee_bps · 1e-4 ;  cash -= fee
turnover += N
equity = cash + inventory · mid      (mark-to-market, старт с нуля)
```

Доказательство — [metrics/pnl_tracker.cpp](../src/bt/metrics/pnl_tracker.cpp):

```cpp
void PnLTracker::on_fill(const Fill& fill) {
  double px = to_price(fill.price), notional = fill.qty*px, fee = notional*fee_bps_*1e-4;
  if (fill.side == Side::Buy) { cash_ -= notional; inventory_ += fill.qty; }
  else                        { cash_ += notional; inventory_ -= fill.qty; }
  cash_ -= fee;  fees_ += fee;  turnover_ += notional;  ++fills_;
}
void PnLTracker::on_mark(Ts ts, double mid) { last_mid_ = mid; last_ts_ = ts; }

PnlReport PnLTracker::report() const {
  r.equity = cash_ + inventory_ * last_mid_;   // <- ключевая величина «equity_pnl»
  ...
}
```

Интерфейс наблюдателя — [metrics/metrics.hpp](../include/bt/metrics/metrics.hpp):
`on_fill`, `on_mark`, `finalize`. Любой компонент, реализующий его, можно подключить к
движку.

---

## 9. Временной ряд и графики

**Простыми словами.** Кроме итоговой строки, можно записывать **временной ряд**
(цена, equity, инвентарь, оборот, объём) с фиксированным шагом по рыночному времени —
чтобы потом нарисовать графики. Чтобы движок (он держит один `Metrics&`) мог кормить
сразу и счётчик PnL, и писателя ряда, есть «разветвитель» `MetricsFanout`.

Доказательство — [metrics/metrics_fanout.hpp](../include/bt/metrics/metrics_fanout.hpp):
он рассылает каждый колбэк всем наблюдателям **в порядке регистрации**. Поэтому
`PnLTracker` добавляют первым (источник истины), а `TimeSeriesRecorder` — вторым, чтобы он
видел уже обновлённое состояние.

`TimeSeriesRecorder` ([…/time_series_recorder.cpp](../src/bt/metrics/time_series_recorder.cpp))
сэмплирует состояние каждые `series_interval_ms` рыночного времени и копит «ведро»
объёма между строками, чтобы объёмный ряд был честной суммой, а не подвыборкой:

```cpp
void on_fill(const Fill& f)  { bucket_qty_ += f.qty; ++bucket_fills_; }   // копим между строками
void on_mark(Ts ts, double mid) {
  if (!started_) { next_sample_ = ts + interval_; snapshot(ts, mid); return; }
  if (ts >= next_sample_) { snapshot(ts, mid); do next_sample_+=interval_; while (ts>=next_sample_); }
}
```

Рисование — отдельный Python-проект в [viz/](../viz/) (`make plot`, `make plots`).
C++ остаётся быстрым и детерминированным; pandas+matplotlib только рисуют CSV.

### Риск-метрики (RiskMetrics)

**Простыми словами.** Кроме PnL нужны метрики *качества* результата. Их считает ещё один
наблюдатель `RiskMetrics` ([risk_metrics.cpp](../src/bt/metrics/risk_metrics.cpp)), тоже
подключённый через `MetricsFanout` (читает equity/инвентарь из `PnLTracker`). В каждый
`report.csv` он добавляет:

- **`max_drawdown`** — максимальная просадка equity-кривой (пик-минус-впадина), в единицах PnL.
- **`return_over_maxdd`** — отношение итогового PnL к просадке (в духе коэффициента Калмара):
  ограниченная и интерпретируемая «доходность на риск».
- **`maker_fill_share`** — доля пассивных (мейкерских) исполнений среди всех (по флагу `Fill.maker`).
- **`inv_mean` / `inv_std` / `inv_max_abs`** — распределение инвентаря по времени (среднее,
  СКО, максимум по модулю — последний показывает реальный размах позиции, который скрыт в
  «инвентаре на конец»).

**Почему не Sharpe.** Движок — оверлей price-taker со стартом от нуля, поэтому его
equity-кривая почти детерминирована (гладкая переоценка инвентаря + детерминированные
комиссии). У такого ряда дисперсия приращений крошечная, и годовой Sharpe «взрывается» до
бессмысленных величин при любом шаге сэмплирования. Поэтому риск-метрикой служит
**доходность/просадка**, а не Sharpe (см. комментарий в `risk_metrics.hpp`).

Сэмплирование — в рыночном времени, на той же сетке, что и ряд; вычисления потоковые
(без хранения строк).

---

## 10. Стратегия 1 — FixedSpreadQuoter (база)

**Простыми словами.** Самый простой маркет-мейкер: всегда держит один бид и один аск
ровно на `half_spread` тиков по обе стороны от середины. Это «эталон-заглушка», чтобы
проверить весь конвейер и иметь, с чем сравнивать умные стратегии. Есть жёсткий лимит
позиции: достигнув его, сторона, наращивающая позицию, перестаёт котироваться.

**Математика** проста:

```
bid = mid - half_spread ;  ask = mid + half_spread
котировать bid, если inventory < +cap ; котировать ask, если inventory > -cap
```

Доказательство — [fixed_spread_quoter.cpp](../src/bt/strategy/fixed_spread_quoter.cpp):

```cpp
Ticks mid = (book.best_bid() + book.best_ask()) / 2;
Ticks target_bid = mid - p_.half_spread, target_ask = mid + p_.half_spread;
bool want_bid = inventory_ <  p_.max_inventory - kEps;   // есть куда покупать
bool want_ask = inventory_ > -p_.max_inventory + kEps;   // есть куда продавать
```

Важная деталь, общая для всех стратегий: **перекотировка без «дёрганья»** — заявка
снимается и переставляется, только если целевая цена изменилась (`target != current_px`),
иначе остаётся стоять (и не теряет место в очереди). Учёт исполнений в `on_fill`: при
полном исполнении сторона помечается «свободной» и перевыставится на следующем
обновлении.

---

## 11. Стратегия 2 — Avellaneda–Stoikov (2008)

Статья: *High-frequency trading in a limit order book*
([pdf](papers/high-frequency-trading-in-a-limit-order-book.pdf)).
Код: [avellaneda_stoikov.hpp](../include/bt/strategy/avellaneda_stoikov.hpp),
[avellaneda_stoikov.cpp](../src/bt/strategy/avellaneda_stoikov.cpp).

### Простыми словами

Фиксированный спред игнорирует две вещи: (1) риск *уже накопленной позиции* и (2)
*волатильность*. A-S чинит обе.

- Если мы **в лонге** (купили лишнего), модель **сдвигает обе котировки вниз**: бид
  становится менее агрессивным, аск — более агрессивным, чтобы охотнее разгрузить лонг.
  Этот сдвиг центра называется **резервной (индифферентной) ценой**.
- Чем выше **волатильность** и чем дальше до конца сессии, тем **шире спред** — нас
  тяжелее «переехать».

### Математика

Середина моделируется как броуновское движение `S_t = s + σ·W_t`. Дилер с позицией `q`
максимизирует экспоненциальную полезность конечного капитала. Решение — два шага:

```
(1) Резервная цена (сдвиг центра против позиции), eq. 3.8:
        r = s − q · γ · σ² · (T − t)

(2) Оптимальный (симметричный) полуспред, eqs. 3.10–3.12:
        δ = ½ · γ · σ² · (T − t)  +  (1/γ) · ln(1 + γ/k)

    Котировки:  bid = r − δ ,  ask = r + δ
```

Где:
- `γ` (gamma) — **неприятие риска**: больше γ ⇒ сильнее сдвиг против позиции и шире спред
  ⇒ теснее держится инвентарь. Это пользовательский параметр (предпочтение).
- `σ²` — дисперсия середины в единицу времени (волатильность).
- `k` — скорость затухания потока исполнений `λ(δ) = A·e^{−kδ}`: как быстро падает шанс
  исполнения с удалением от середины.
- `(T − t)` — время до конца сессии. Первый член спреда растёт с волатильностью и
  горизонтом; второй — «ликвидная премия», пол спреда, заданный `k`.

Смысл: позиция `q` тянет центр в сторону, уменьшающую риск — мейкер агрессивнее на той
стороне, что *снижает* позицию, и осторожнее на той, что её *наращивает*. Именно этого
контроля нет у фиксированного спреда.

### Доказательство из кода

Эта реализация **дословно следует статье** — три принципиальных выбора зафиксированы в
комментарии заголовка и в коде:

```cpp
double tau = std::max(0.0, double(p_.horizon_us - (now - t0_)) * 1e-6);   // (T − t), один раз вниз до 0
double center = center_price(book);          // mid (или микропрайс в наследнике)
double sigma2 = p_.sigma * p_.sigma;          // ПОСТОЯННАЯ дисперсия (см. калибровку)
double q = inventory_ / p_.order_qty;         // позиция в «лотах»

double reservation = center - q * p_.gamma * sigma2 * tau;          // eq. 3.8
double half        = 0.5 * p_.gamma * sigma2 * tau;
if (p_.k > 0 && p_.gamma > 0)
  half += (1.0 / p_.gamma) * std::log1p(p_.gamma / p_.k);            // eqs. 3.10–3.12

Ticks target_bid = to_ticks(reservation - half);
Ticks target_ask = to_ticks(reservation + half);
// БЕЗ лимита позиции и БЕЗ пола спреда — обе стороны всегда стоят на r ± δ.
```

Три «каноничности» (и почему это важно):
1. **σ и k — постоянные**, откалиброваны один раз офлайн (раздел 15), без онлайн-подстройки.
2. **Единственный горизонт `T`**: `(T − t)` отсчитывается один раз от первого события и
   упирается в 0 (терминальное время статьи) — **без перезапуска**.
3. **Нет лимита позиции и нет пола спреда** — инвентарь должен контролировать сама функция
   полезности.

Единственное неизбежное отклонение от непрерывной модели — **округление котировок к сетке
тиков** (`to_ticks`).

> ⚠️ **Следствие на длинной ленте.** При едином горизонте член `(T − t)` обнуляется через
> `T` секунд, и дальше ~6 дней котировки симметричны, а инвентарь не контролируется и
> «уплывает» с трендом. Это честный, верный статье результат — но именно поэтому для
> практики сделана стратегия 4 (раздел 13). Подробный разбор — в [STRATEGY.md](STRATEGY.md).

---

## 12. Стратегия 3 — Micro-price (Stoikov 2018)

Статья: *The Micro-Price: A High Frequency Estimator of Future Prices*
([pdf](papers/the-micro-price-a-high-frequency-estimator-of-future-prices.pdf)).
Код: [microprice.hpp](../include/bt/strategy/microprice.hpp),
[microprice.cpp](../src/bt/strategy/microprice.cpp),
[microprice_as.hpp](../include/bt/strategy/microprice_as.hpp).

### Простыми словами

Обычная середина `mid` — плохой предсказатель того, *куда* пойдёт цена дальше. Если биды
заметно «тяжелее» асков, следующий тик обычно вверх. Объёмо-взвешенная середина это
учитывает, но **переусердствует** и, главное, **не является мартингалом** (нельзя
использовать как честную справедливую цену).

**Микропрайс** Стойкова — это предел ожидаемых будущих середин. По построению он
мартингал, поэтому это корректная справедливая цена:

```
P^micro = M + g(I, S)
```

— это `mid` плюс поправка `g`, зависящая только от двух наблюдаемых величин верха стакана:
**дисбаланса** `I = Qb/(Qb+Qa)` и **спреда** `S`.

### Математика: как считается g

Дискретизуем `(I, S)` в конечный набор состояний и смотрим на стакан как на **марковскую
цепь**. Из данных строим:

```
Q        — переходы, на которых mid НЕ сдвинулся (блуждание внутри, «transient»)
T        — переходы, на которых mid сдвинулся (скачок в новое состояние)
rvec[x]  — E[ΔM · 1(mid сдвинулся) | состояние x]      (произведение R·K из статьи)

G1 = (I − Q)⁻¹ · rvec        — поправка 1-го порядка (свернули всё блуждание без сдвига)
B  = (I − Q)⁻¹ · T           — куда попадаем (в пространстве состояний) после сдвига
G* = Σ_{i≥0} Bⁱ · G1         — полная поправка (геометрический ряд по последующим сдвигам)
```

`(I + Q + Q² + …) = (I − Q)⁻¹` — это и есть аналитическое сворачивание бесконечного
блуждания без сдвига. `g(I,S) = G*[состояние]`.

**Симметризация.** Каждый образец `(I, S, ΔM)` зеркалится в `(1−I, S, −ΔM)`. Это убирает
направленный сдвиг и гарантирует сходимость ряда (Теорема 3.1: `B*·G1 = 0`).

### Доказательство из кода

Накопление переходов с симметризацией — [microprice.cpp](../src/bt/strategy/microprice.cpp):

```cpp
void MicropriceModel::add_transition(double imb, Ticks spread, double imb_next,
                                     Ticks spread_next, double dM, bool moved) {
  ...
  accumulate(imb, spread, imb_next, spread_next, dM, moved);
  accumulate(1.0 - imb, spread, 1.0 - imb_next, spread_next, -dM, moved);  // зеркало (I -> 1-I)
}
```

Решение `(I−Q)` (Гаусс–Жордан с частичным выбором главного элемента, общая факторизация
для `rvec` и `T`) и итеративная сумма ряда `G*`:

```cpp
// (I−Q) X = [rvec | T]  одной факторизацией:
solve_in_place(a, rhs_mat, n, rhs);
// G* = Σ Bⁱ G1, пока член не станет пренебрежимо мал:
g_star_ = g1; term = g1;
for (it=0; it<1000; ++it) {
  next = B * term;  g_star_ += next;  term.swap(next);
  if (maxabs(next) < 1e-12) break;          // сходимость
}
```

Запрос поправки; для спредов вне модели — откат к `mid` (поправка 0):

```cpp
double MicropriceModel::adjustment(double imbalance, Ticks spread) const {
  if (!fitted_ || nm_ == 0) return 0.0;
  if (spread < 1 || spread > cfg_.n_spread) return 0.0;   // вне состояний модели -> mid
  return g_star_[state(imbalance, spread)];
}
```

**Дискретно-временная сетка** (Section 4 статьи): переходы берутся между точками фикс.
сетки времени (`sample_dt_us`, ~1 с), а не на каждое обновление стакана. Калибровка —
один проход по LOB (`MicropriceModel::calibrate`).

### Как это вплетается в A-S

`MicropriceAS` наследует всю стратегию A-S и **переопределяет только центр котировок** —
[microprice_as.hpp](../include/bt/strategy/microprice_as.hpp):

```cpp
double center_price(const OrderBook& book) const override {
  double qb = book.best_bid_qty(), qa = book.best_ask_qty();
  double imb = (qb+qa > 0) ? qb/(qb+qa) : 0.5;
  return book.mid() + model_.adjustment(imb, book.spread());   // mid -> mid + g(I,S)
}
```

То есть резервная цена A-S теперь строится не вокруг `mid`, а вокруг `mid + g(I,S)` —
поверх инвентарного сдвига добавляется упреждение дрейфа, вызванного дисбалансом. Всё
остальное (спред, контроль позиции, калибровка σ/k) — без изменений.

---

## 13. Стратегия 4 — Avellaneda–Stoikov Online

Код: [avellaneda_stoikov_online.hpp](../include/bt/strategy/avellaneda_stoikov_online.hpp),
[avellaneda_stoikov_online.cpp](../src/bt/strategy/avellaneda_stoikov_online.cpp),
оценщики — [online_estimators.hpp](../include/bt/strategy/online_estimators.hpp).

### Простыми словами

Каноничная A-S (раздел 11) верна, но на непрерывной многодневной ленте непрактична:
инвентарный сдвиг «умирает» после `T`, и позиция уплывает. Эта стратегия — **рабочий
маркет-мейкер с нормальным инвентарём**. Она оставляет ту же формулу, но смягчает ровно
три вещи, которые мешают применять её вживую.

### Что именно меняется (три отличия)

1. **Скользящий горизонт θ (онлайн-перебалансировка).** `(T − t)` берётся **по модулю**
   длины сессии `T`: он «пилит» в диапазоне `[0, T]` и **перезапускается** каждую сессию,
   а не упирается в 0. Поэтому член `q·γ·σ²·(T − t)` жив весь прогон — именно это и держит
   инвентарь.
2. **Онлайн σ и k.** Обе величины непрерывно переоцениваются EWMA-фильтрами (а не
   фиксируются офлайн), поэтому котировки адаптируются к смене режима. Офлайн-значения
   служат «затравкой».
3. **Лимит позиции + пол спреда.** Жёсткий `max_inventory` снимает котировку со стороны,
   которая нарастила бы позицию сверх лимита; полуспред не опускается ниже
   `min_half_spread` тиков. Базовые риск-контроли, которых нет в непрерывной модели статьи.

### Математика онлайн-оценок

```
Скользящий горизонт:   phase = (now − t0) mod T ;   τ = T − phase           (пила в [0, T])

Волатильность (EWMA):  σ²_per_sec = EWMA(ΔS²) / EWMA(Δt)
   — два EWMA по отдельности, чтобы микросекундные Δt не «взрывали» мгновенное отношение.

Поток исполнений (EWMA): k = 1 / EWMA(|сделка − mid|)
   — расстояние сделки от середины распределено экспоненциально с темпом k (как в статье).
```

Резервная цена и полуспред — те же eq. 3.8 / 3.10–3.12, что и в разделе 11.

### Доказательство из кода

Скользящий горизонт и онлайн-параметры — [avellaneda_stoikov_online.cpp](../src/bt/strategy/avellaneda_stoikov_online.cpp):

```cpp
double mid = book.mid();
vol_.update(mid, now);                                    // обновляем онлайн σ

Ts phase = (p_.horizon_us > 0) ? ((now - t0_) % p_.horizon_us) : 0;
double tau = double(p_.horizon_us - phase) * 1e-6;        // <- ПИЛА: θ перезапускается каждую сессию

double sigma2 = vol_.ready() ? vol_.sigma2_per_sec() : (p_.seed_sigma*p_.seed_sigma);
double k = current_k();                                   // онлайн k (или затравка)
double q = inventory_ / p_.order_qty;

double reservation = mid - q * p_.gamma * sigma2 * tau;   // eq. 3.8
double half = 0.5 * p_.gamma * sigma2 * tau;
if (k>0 && p_.gamma>0) half += (1.0/p_.gamma) * std::log1p(p_.gamma/k);   // 3.10–3.12
```

Пол спреда и лимит позиции:

```cpp
if (target_ask - target_bid < 2*min_half) {              // пол спреда
  target_bid = std::min(target_bid, mid_tick - min_half);
  target_ask = std::max(target_ask, mid_tick + min_half);
}
bool want_bid = inventory_ <  p_.max_inventory - kEps;   // лимит позиции
bool want_ask = inventory_ > -p_.max_inventory + kEps;
```

Онлайн k от сделок (в `on_trade`):

```cpp
void AvellanedaStoikovOnline::on_trade(const TradePrint& trade, const OrderBook& book, ...) {
  if (book.valid()) arr_.update(std::abs(to_price(trade.price) - book.mid()));   // расстояние до mid
}
```

Сами EWMA-оценщики — [online_estimators.hpp](../include/bt/strategy/online_estimators.hpp):

```cpp
// Волатильность: два EWMA, σ²/сек = EWMA(ΔS²)/EWMA(Δt)
void VolatilityEstimator::update(double mid, Ts ts) {
  if (have_) { double d=mid-last_mid_, dt=double(ts-last_ts_)*1e-6;
    if (dt>0){ ewma_d2_ += alpha_*(d*d - ewma_d2_); ewma_dt_ += alpha_*(dt - ewma_dt_); ready_=true; } }
  last_mid_=mid; last_ts_=ts; have_=true;
}
double sigma2_per_sec() const { return ewma_dt_>0 ? ewma_d2_/ewma_dt_ : 0.0; }

// Поток исполнений: k = 1 / EWMA(|сделка − mid|)
void ArrivalRateEstimator::update(double delta){ ewma_delta_ += alpha_*(std::max(delta,min_delta_)-ewma_delta_); ready_=true; }
double k() const { return ewma_delta_>0 ? 1.0/ewma_delta_ : 0.0; }
```

**Результат:** инвентарь держится у нуля (≈ −3 262 при γ=500 против ~478 000 у каноничной
версии). Это «нормальная» стратегия, о которой шла речь.

---

## 14. Стратегия 5 — Micro-price + Online A-S (лучшая)

Код: [microprice_as_online.hpp](../include/bt/strategy/microprice_as_online.hpp).

### Простыми словами

Две предыдущие идеи по отдельности «не дотягивают»:
- **Каноничный micro-price A-S (раздел 12)** упреждает дрейф от дисбаланса, но отпускает
  инвентарь до ~910 000 (единый горизонт, без лимита).
- **Online A-S (раздел 13)** держит инвентарь у нуля, но котируется вокруг обычного `mid`
  и потому игнорирует тот самый краткосрочный дрейф, который предсказывает микропрайс.

Эта стратегия объединяет их: берёт **все контроли** Online A-S (скользящий горизонт,
онлайн σ/k, лимит позиции, пол спреда) и меняет **только центр котировок** — вместо `mid`
ставит микропрайс `M + g(I,S)`. Получаем **и управляемый инвентарь, и упреждение дрейфа**
одновременно.

### Математика

```
Резервная цена:  r = (M + g(I,S)) − q · γ · σ² · (T − t)        со СКОЛЬЗЯЩИМ (T − t)
Полуспред:       δ = ½ γ σ² (T − t) + (1/γ) ln(1 + γ/k)         (как в разделах 11/13)
```

То есть всё как в Online A-S, но центр сдвинут на поправку микропрайса `g(I,S)`.

### Доказательство из кода

В коде это — переопределение **одного метода** базового Online A-S
([microprice_as_online.hpp](../include/bt/strategy/microprice_as_online.hpp)):

```cpp
class MicropriceASOnline final : public AvellanedaStoikovOnline {
  double center_price(const OrderBook& book) const override {   // mid -> mid + g(I,S)
    double qb = book.best_bid_qty(), qa = book.best_ask_qty();
    double imb = (qb+qa > 0) ? qb/(qb+qa) : 0.5;
    return book.mid() + model_.adjustment(imb, book.spread());
  }
  MicropriceModel model_;
};
```

Базовый класс был для этого расширен: его `on_book` теперь строит резервную цену вокруг
`center_price(book)` (виртуальный метод), а волатильность по-прежнему считает по реальному
`mid` — [avellaneda_stoikov_online.cpp](../src/bt/strategy/avellaneda_stoikov_online.cpp):

```cpp
const double center = center_price(book);              // mid (или микропрайс в наследнике)
const double reservation = center - (q * p_.gamma * sigma2 * tau);
```

Конфиг: `strategy: "microprice_as_online"`
([configs/microprice_as_online.json](../configs/microprice_as_online.json)) — принимает и
онлайн-параметры (`as_*`), и параметры микропрайса (`mp_*`).

### Результат

При γ=500: инвентарь −5 594, PnL −368.0 — это **на ~7% лучше**, чем у Online A-S (−396.4)
при почти таком же профиле исполнений. Микропрайс начинает приносить пользу именно тогда,
когда инвентарь под контролем (в каноничной версии этот эффект тонул в неконтролируемом
дрейфе). По свипу γ эта стратегия **обыгрывает Online A-S при любом γ ≥ 10**, а лучшая
точка по всему набору — **γ ≈ 2000** (PnL −261.6, инвентарь −973). Это рекомендуемая
стратегия.

---

## 15. Калибровка параметров

Есть **три** механизма калибровки — у каждого своя роль.

### 15.1. Офлайн σ и k (для `as` / `microprice_as`)

Один проход по объединённой ленте *до* прогона, затем значения держатся постоянными —
[avellaneda_stoikov.cpp](../src/bt/strategy/avellaneda_stoikov.cpp):

```
σ² = Σ(ΔMid)² / Σ Δt        (дисперсия середины в секунду)
k  = 1 / mean(|сделка − mid|) (темп экспоненциального затухания исполнений)
```

```cpp
ASConstants AvellanedaStoikov::calibrate(EventSource& feed) {
  // по BookUpdate: sum_d2 += (mid - last_mid)^2 ; sum_dt += dt
  // по TradePrint: sum_dist += |trade - last_mid| ; ++n_trades
  c.sigma = sqrt(sum_d2 / sum_dt);
  c.k     = 1.0 / (sum_dist / n_trades);
}
```

На полных данных получается `σ ≈ 2.88e-6`, `k ≈ 3.99e5` (печатается в stderr при запуске).

### 15.2. Онлайн σ и k (для `as_online` / `microprice_as_online`)

EWMA-оценщики из раздела 13: непрерывная переоценка, «затравка» — из офлайн-калибровки
(см. [apps/backtest.cpp](../apps/backtest.cpp), ветки `as_online` / `microprice_as_online`).

### 15.3. Калибровка микропрайса (для `microprice_as` / `microprice_as_online`)

Один проход по LOB строит марковскую цепь и решает её (раздел 12). В
[apps/backtest.cpp](../apps/backtest.cpp) это `MicropriceModel::calibrate(...)`; на полных
данных — ~457 776 переходов.

---

## 16. Результаты на полных данных

Полный набор: **22 901 679 событий** (1 036 690 обновлений стакана, 21 864 989 сделок),
~6 дней. `fee_bps = 1.0`, `order_latency_us = 1000`, `order_qty = 1000`,
`max_inventory = 100 000`, оптимистичная очередь. Воспроизведение: `make experiments`
(+ `make sweep` для свипа по γ).

Итоги при γ = 500:

| Стратегия | Исполн. | Инвентарь | Оборот | Equity PnL | Max DD | Ret/DD | Max\|inv\| |
|--|--:|--:|--:|--:|--:|--:|--:|
| Fixed (база) | 764 349 | −88 008 | 5.43M | −1525.9 | 1532.5 | −1.00 | 100 999 |
| A-S (каноничная) | 173 921 | 478 255 | 1.28M | +1486.0 | 399.6 | +3.72 | 666 960 |
| Micro-price + A-S | 173 841 | 909 931 | 1.27M | +1194.4 | 1007.2 | +1.19 | 1 045 400 |
| A-S Online | 260 181 | **−3 262** | 1.95M | −396.4 | 396.5 | −1.00 | 21 337 |
| **Micro-price + Online A-S** | 248 813 | **−5 594** | 1.88M | **−368.0** | **368.1** | −1.00 | 24 832 |

*Max DD* — макс. просадка equity (ед. PnL); *Ret/DD* — PnL ÷ просадка (доходность на риск);
*Max|inv|* — макс. позиция по модулю.

- Высокий «PnL» у каноничных версий — это в основном **уплывшая длинная позиция**,
  переоценённая на растущей ленте (направленная ставка, не маркет-мейкерская премия).
  Их `Max|inv|` (0.67–1.0 млн) это и выдаёт.
- **Обе онлайн-версии** держат инвентарь у нуля (`Max|inv|` ≤ 25k из лимита 100k),
  поэтому их PnL — честная стоимость предоставления ликвидности на этом «оверлее».
- **Микропрайс помогает, когда инвентарь под контролем:** версия §14 лучше Online A-S на
  ~7% (−368.0 против −396.4) и с меньшей просадкой (368 против 396).
- Свип по γ: для каноничных версий γ почти не влияет на инвентарь (эффект единого
  горизонта); для онлайн-версий γ — реальный контроль, и **Micro-price + Online A-S
  обыгрывает Online A-S при любом γ ≥ 10** (лучшая точка — γ ≈ 2000: PnL −261.6,
  инвентарь −973). Полные таблицы — в [STRATEGY.md](STRATEGY.md).

---

## 17. Допущения, ограничения, тесты

**Допущения модели (важно для интерпретации цифр):**
- **Price-taker overlay** — наши заявки не двигают ленту; нет рыночного импакта и
  адверс-селекшна сверх того, что уже в данных. ⇒ абсолютный PnL оптимистичен,
  сравнение стратегий — надёжно.
- **Очередь — оценка** (есть только агрегированный L2). Две модели: `optimistic`
  (верхняя граница исполнений) и `proportional` (консервативнее).
- **Латентность постоянна**; `feed_latency` пока зарезервирована.
- **Цена исполнения мейкера** — наша цена заявки; тейкера — цена уровней стакана.

**Тесты (36, все зелёные — `make test`).** Каждая формула из статей покрыта проверкой:
- A-S: симметрия котировок при нулевой позиции; сдвиг вниз в лонге и отсутствие лимита;
  обнуление сдвига после `T` (единый горизонт); **точный полуспред** (eqs. 3.10–3.12);
  **восстановление σ и k** в `calibrate`.
- Micro-price: предсказание дисбаланса; нулевая поправка при неинформативном состоянии;
  симметризация; сетка сэмплирования; отбрасывание спредов вне модели; центр
  `MicropriceAS` = `mid + g(I,S)`.
- A-S Online: скользящий горизонт сохраняет сдвиг после `T`; срабатывание лимита позиции;
  онлайн-переоценка `k` от сделок.
- Micro-price + Online A-S: центр котировок = `mid + g(I,S)` при сохранённых
  онлайн-контролях.
- RiskMetrics: расчёт просадки, доходности/просадки и распределения инвентаря; доля
  мейкерских исполнений.
- Плюс тесты ядра, данных, стакана, исполнения, очереди, движка и метрик.

**Дорожная карта** (детали — в [STRATEGY.md](STRATEGY.md) и [ARCHITECTURE.md](ARCHITECTURE.md)):
полная конечно-горизонтная θ-PDE (асимметричные `δ`), многоуровневые/размерные котировки,
авто-подстройка γ (σ/k уже онлайн), диагностика адверс-селекшна,
латентность фида и рыночный импакт для доверия к абсолютному PnL.

---

*Файлы кода кликабельны (пути относительно корня репозитория). Для формул см. статьи в
[docs/papers/](papers/).*
