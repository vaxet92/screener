# DESIGN.md — Bybit USDT perpetual screener

Current architecture, invariants, implementation status, and rejected alternatives.
`screener_prompt.md` holds the assignment, the working mode (DRILL/SCAFFOLD) and the
2-day roadmap; this file holds the design those steps implement.

Status legend: **[done]**, **[partial]**, **[not implemented]**, **[open question]**.

Venue facts marked **[verified 2026-10-09]** were read off the Bybit v5 documentation;
anything marked **[unverified]** is an assumption that must be confirmed before the code
depends on it.

---

## 1. What the system does

Watches 500–1000 **Bybit USDT perpetual futures** (`category=linear`,
`contractType=LinearPerpetual`) and marks each symbol `ACTIVE` or `INACTIVE` on every
closed 1-hour candle. It also ranks the ACTIVE set by NATR.

Three conditions, all of which must hold for ACTIVE (thresholds in `config.json`):

| Condition | Definition | Default |
|---|---|---|
| Volume surge | (Σ turnover of last 24 closed LTF bars) / (Σ of the 24 before) − 1 | > 30 % |
| Trend | last LTF close > EMA50 on HTF bars | true |
| Volatility | NATR = ATR(14, Wilder, LTF) / close × 100 | ≥ 1.0 % |

**LTF = 1h** (subscribed) and **HTF = 4h** (aggregated locally, §5).

A symbol with too little history is **`not ready`** and can never be ACTIVE. Readiness is
per signal, not global (§6).

This is a **screener, not an order-book system**. There is no book, no BBO, no venue
consolidation, and no sub-millisecond requirement — a decision is due within milliseconds
of an hourly close, not microseconds of a tick. The latency work is justified by the
exercise list, not by the business requirement, and the debrief should say so plainly.

---

## 2. Processes, threads and ownership

**MVP: one process, ONE thread, no queues.** The provider hands each parsed candle
straight to the core through a callback. Threads and queues arrive in Phase 1 (§13) only
if a measurement asks for them.

```
main thread, one io_context
  ControlManager::Run()                                   [control_manager/]
    1. REST instruments-info (cursor paged) -> symbol universe -> ids   [BLOCKING]
    2. WS connect + batched subscribe                     [md_provider/]
    3. async REST kline interval=60 per symbol -> CoreManager::Warmup()  [LTF ONLY, see 7]
       16 in flight, paced by RateLimiter::AsyncAcquire
    4. ioc.run() drives 2 and 3 concurrently:
         WS frame before warm-up ends -> startup_buffer_   <-- see "Startup buffer" below
         last warm-up response -> replay the buffer -> warmed_up_ = true
         WS frame after that -> parse -> Candle -> CoreManager::ApplyCandle()  <-- direct callback
         20s ping timer -> send {"op":"ping"}, check pong freshness
         after each frame -> drain pending rebuilds (async REST)

  CoreManager                                             [md_core/]
    dedup/gap by open_time -> LTF tracker -> HTF aggregate -> HTF tracker
    -> TryActivate/TryDeactivate -> console table
```

### Invariants

1. **One thread owns everything.** `CoreManager` state is written only from the
   `io_context` thread, which is also the only thread that exists. The callback is safe
   *because* of this, not by accident — see the warning below.
2. **`open_time` is the only identity of a bar.** Dedup, gap detection and HTF alignment
   all key on it. See §4.
3. **A rebuild is never performed inline.** `ApplyCandle` detecting a gap records the
   symbol id and returns; the main loop drains the pending list afterwards. See §7.
4. **Zero heap allocation on the message path after startup** is a **Phase 1 goal**, not
   an MVP invariant, and it is verified by a malloc-counting test rather than by
   inspection (§13). The MVP uses ordinary containers on purpose, so there is a baseline
   to measure against.
5. Symbol **strings never appear on the message path**. A `Candle` carries a `uint32_t`
   symbol id; the string exists only in config, logging and the active set.

### Why a callback and not an SPSC queue

One thread makes the queue pointless: a lock-free ring between a producer and a consumer
that are the same thread is pure overhead and one more thing to explain. One `io_context`
drives any number of WebSocket sessions asynchronously, so "many sockets" does not imply
"many threads".

The capacity argument: the confirmed-bar rate is ~1000 **per hour**, and the unconfirmed
frames we discard are at most ~1000 **per second** (§8). Even at 5 µs per message that is
0.5 % of one core. There is no throughput problem to solve.

> **The callback is only safe while there is one thread.** The moment a provider runs on
> its own thread, a direct call into `CoreManager` becomes a data race on every signal
> manager — K threads mutating the same EMA state. That is the point where the callback
> must become an SPSC push, and it is not a refactor to do casually. `md_core/spsc_queue.h`
> stays in the tree for exactly that step (Phase 1, §13).

**What the MVP gives up by being single-threaded:** no parse parallelism, and no
"prove a restart cannot produce two concurrent producers" story — that argument has
nothing to prove when there is one thread. Phase 1 restores both, with a measurement
justifying the thread. Worth knowing before the debrief: this is a deliberate trade, not
an oversight.

### Blocking REST on the io_context thread

Only **one** REST call blocks now: `instruments-info`, in `FetchUniverse`. It runs before
`ioc.run()` is entered, so there is no event loop to stall, and nothing could overlap with
it anyway — ids come from its result, so neither the subscribe nor the warm-up can start
until it returns.

Everything after that is async (`md_provider/async_rest.h`), because the warm-up now
**overlaps the live stream** and blocking the one thread would starve exactly the socket
the overlap exists to protect. `RateLimiter` therefore has two interfaces over one window:
`Acquire()` (blocking, startup only) and `AsyncAcquire()` (a handler posted to the
`io_context` when a slot frees). Two separate windows would let the two paths together
exceed the IP limit.

**Measured** (2026-10-09, 791 symbols, 603 bars each, 16 concurrent): warm-up completes in
**17.98 s**, ~44 req/s. The WS handshake and all 8 subscribe acks land *inside* that
window, which is the overlap working. At 16 concurrent and 44 req/s a single request takes
~364 ms, so the sequential equivalent would be ~288 s — the speedup is the concurrency
factor, which means the **rate limiter is not yet the binding constraint** (its ceiling is
60 req/s at the default 300-per-5s).

**Known MVP limitation:** a rebuild storm no longer stalls the loop, but ~800 symbols
gapping at once would still queue ~800 requests behind the limiter at 60 req/s ≈ 13 s
before the last one is served. That is a pacing floor, not a stall — frames keep being
read throughout.

### Startup buffer

A closed bar is pushed **exactly once**. If it closes while we are not yet subscribed, it
is gone — and `Classify` only notices on that symbol's *next* bar, up to an hour later, so
the symbol runs one bar stale in the meantime with internally consistent but lagging
indicators. Against a 3600 s bar period an 18 s warm-up means this happens in roughly
**0.5 % of runs**, hitting every symbol warmed before the boundary.

So the subscribe comes **first** and `ControlManager::OnCandle` buffers into
`startup_buffer_` until `warmed_up_`. `FinishWarmup` then replays it in arrival order
through `ApplyCandle`.

**The reconciliation needs no new code.** `open_time` is already a bar's only identity, so
a buffered bar the REST history also covers is `kDuplicate`/`kStale` and is dropped, and
one past it is `kNext` and is applied. This is the snapshot-plus-delta pattern used to
bootstrap an order book, except that klines have no sequence number to reconcile against —
`open_time` *is* the sequence, which is why there is no reorder buffer and no "wait for
seq > snapshot" state machine.

Sizing: at 1h bars an 18 s warm-up yields at most one bar per symbol, so ~800 candles
(~38 KB) is the realistic worst case. `kMaxStartupBuffer = 65536` is three orders of
magnitude of slack; overflowing it means warm-up has been running for hours, and dropping
those bars is correct because the gap path rebuilds the affected symbols anyway.

**Measured:** both live runs replayed **0** buffered candles — no hour boundary fell
inside the 18 s window, as expected at 0.5 %. The buffer-and-replay path is therefore
**exercised by no test and by no live run yet**, which is its main weakness: it needs
either a ControlManager test with an injected REST source, or a run started deliberately
~20 s before the hour.

**Rejected:** keeping the old order (warm up, then subscribe) and letting the gap path
absorb it. It does self-heal, but detection is up to an hour late and the recovery is a
full 603-bar refetch for potentially half the universe at once.

**Rejected:** a per-symbol buffer. Arrival order across symbols is irrelevant because
symbols are independent, and within a symbol the stream is already ordered, so one flat
vector in arrival order is sufficient and allocates once.

### Why one core thread even later

1000 symbols × one closed bar per hour ≈ 1000 updates/hour, bursting over a few seconds at
the hour boundary. The per-bar work is a history push, two running-sum updates, two
recursive indicator steps and three integer comparisons. Even multi-threaded, the *core*
stays single-threaded: threads would exist for I/O and parsing, never for state.

**Rejected:** a thread per symbol shard with its own state. It would buy nothing
measurable and would require the aggregator, the active set and the console table to
become shared structures.


---

## 3. Numeric representation — scaled integers

**[decided]** Prices and volumes are integers with a fixed multiplier. **There is no
floating point anywhere in this system**, including inside the indicators.

```cpp
using Price  = int64_t;   // real price   x kPriceScale
using Volume = int64_t;   // real turnover x kVolumeScale

inline constexpr int64_t kPriceScale  = 10'000'000'000;  // 1e10
inline constexpr int64_t kVolumeScale = 1'000'000;       // 1e6
```

### Why two different scales

They solve different problems, so one constant cannot serve both.

- **Price: 1e10.** The perp universe spans BTCUSDT at ~120 000 down to the
  1000-multiplier contracts at ~0.000006, a range of ~10^10. The scale has to be large
  enough that the cheapest contract keeps useful precision and small enough that the
  dearest one does not overflow an intermediate.
  - At **1e8** a price of 0.000006 becomes **600 units — three significant digits**. Its
    ATR is then ~6 units and NATR is quantised to ~17 basis points, against a threshold
    of 100 bp. Borderline symbols would flip on rounding alone, and the cheap volatile
    symbols are exactly the ones a surge screener exists to find.
  - At **1e10** the same price is 60 000 units (five significant digits) and NATR is
    quantised to ~0.17 bp. BTC at 120 000 becomes 1.2e15, so the largest intermediate in
    the ATR recursion (`atr * 13`) is ~1.6e16 — 575× clear of the int64 ceiling.
- **Volume: 1e6.** Turnover is always USDT for linear contracts, one currency with a
  bounded range, so it needs no headroom for magnitude — it needs headroom for
  *summation*. A BTCUSDT 1h turnover near 1e9 USDT is 1e15 at this scale, and the 24-bar
  window sum is ~2.4e16. At 1e8 that sum would be 2.4e18, within 4× of overflow; at 1e6
  there is 380× margin. Micro-USDT resolution is far more than a volume ratio needs.

### Where the width runs out

Three places need `__int128`, and each is a real overflow, not a precaution:

| Expression | Magnitude at these scales | Fits int64? |
|---|---|---|
| `atr * 13` (Wilder recursion) | ~1.6e16 | yes |
| `(price - ema) * 2` (EMA50 step) | ~2.4e15 | yes |
| `atr * 10'000 / close` (NATR in bp) | `1.2e15 * 1e4` = **1.2e19** | **no** |
| `recent * 100` vs `prev * 130` (surge) | `2.4e16 * 130` = **3.1e18** | thin — 3× |

So NATR and the surge comparison are computed in `__int128`. The ATR and EMA recursions
stay in `int64_t`.

### Thresholds are integer comparisons

No division, no float, no epsilon:

- **Surge > 30 %:** `(__int128)recent * 100 > (__int128)prev * 130`.
- **NATR >= 1.0 %:** NATR is carried in **basis points** as an integer,
  `natr_bp = (__int128)atr * 10'000 / close`, and compared against `100`. The ratio is
  dimensionless, so `kPriceScale` cancels and the number is comparable across symbols —
  which is what makes the cross-sectional NATR rank meaningful.
- **Trend:** `last_close_ltf > ema50_htf`, both at `kPriceScale`.

### Rounding in the recursive indicators

This is the one genuine cost of going integer, and it is worth being precise about it.

EMA50 and Wilder ATR are recursive, so each step divides and the truncation error feeds
the next step. **Both use rounding division, not truncation** (add half the divisor before
dividing).

The error is bounded, and the bound is small enough to ignore here:

- Each step injects at most 0.5 unit of error.
- The EMA is a contraction — its homogeneous response decays by 49/51 per step — so the
  worst-case accumulated error is `0.5 * sum((49/51)^k) = 0.5 * 51/2 ≈ 13 units`.
- For the *cheapest* symbol (close ≈ 60 000 units) that is **~2e-4 relative, 0.02 %**,
  against a 1 % NATR threshold and a 30 % surge threshold. For every other symbol it is
  smaller.
- With rounding, a price move below ~13 units (0.02 % for the cheapest symbol) does not
  move the EMA at all. Irrelevant at these thresholds, but it is the reason the indicator
  tests compare against a double reference with a **stated tolerance** rather than
  bit-exactly.

**Alternative (not needed):** carrying the division remainder forward (Bresenham style)
removes the accumulated bias entirely for a few bytes of state. Worth mentioning in the
debrief as the zero-bias version; the error budget above does not justify it.

**Alternative (rejected):** `double` prices with integer turnover. Uniform relative
precision and no rounding analysis needed, but it reintroduces floating point into stored
state for a system whose every threshold is an exact integer comparison, and it gives up
the reproducibility that makes a replay test meaningful.

**Alternative (rejected for the MVP):** a per-symbol scale from `instruments-info`
`priceScale` / `priceFilter.tickSize`. Strictly the most correct — it is what an
order-placing system must do — and the NATR rank would still work because the ratio is
dimensionless. But it puts a per-symbol scale into every indicator, every comparison and
every test fixture. A single 1e10 scale already covers the observed range with five
significant digits at the bottom end. Revisit only if a listed symbol prices below ~1e-7.

### Parsing

Bybit sends every price and volume as a **decimal string**, which is what makes the
integer path exact: `md_provider/decimal.h` `ParseScaledDecimal<Scale>` converts straight
to a scaled integer with no floating-point intermediate.

It needs a two-line change: `kMultipliers` currently stops at 1e8 and the template
`static_assert`s `Scale <= 8`, so it must be extended to 1e10. It also returns
`uint64_t`; prices and turnover are non-negative, so the cast to `int64_t` at the parse
boundary is safe and should be explicit.


## 4. The `Candle` type and bar identity

**[done]** — `types/candle.h`. MVP form below; the 64-byte layout is a **Phase 1** exercise
(§13), not an MVP requirement.

One type serves both the queue message and the stored bar.

```cpp
struct Candle {
    int64_t  open_time_ms;       //  8  IDENTITY: dedup, gap detection, HTF alignment
    int64_t  event_time_ms;      //  8  envelope `ts` of the message that CLOSED this bar
    Price    open, high, low, close;  // 32  int64, x kPriceScale
    Volume   turnover;           //  8  int64, x kVolumeScale
    uint32_t symbol_id;          //  4  index into CoreManager's state vector
    uint32_t reserved_;          //  4  explicit, so the padding is a decision
};
static_assert(std::is_trivially_copyable_v<Candle>);
```

It happens to be 64 bytes already, because every field is 8 bytes except the two
`uint32_t`s. **The MVP does not assert that**, and does not reorder fields to defend it.
Phase 1 adds `static_assert(sizeof(Candle) == 64)` together with the benchmark that
shows whether one-cache-line slots actually matter at this message rate — which is the
whole point of the exercise, and a result that could legitimately come back "no
difference".

Bybit's kline payload carries no trade count, so the spare 4 bytes are named
`reserved_` rather than left to the compiler.

`end` (close time) is not stored: it is `open_time_ms + interval - 1`, derivable.

**Why 64 bytes is the Phase 1 target:** a bar is then exactly one cache line, so a ring
slot never straddles two lines and a push touches one. At 72 bytes every other slot
straddles, and the SPSC ring's slot array stops being line-aligned.

### open_time vs event_time

These answer different questions and must not be confused.

- **`open_time_ms` identifies the bar.** It is Bybit's `data[].start`
  **[verified 2026-10-09]**: assigned by the exchange, identical in the WebSocket push and
  in a REST backfill of the same bar, and identical every time the bar is re-sent. That is
  what makes it the dedup key, the gap detector (`open_time == last + interval`?) and the
  HTF alignment key.
- **`event_time_ms` describes the message, not the bar.** It is the envelope `ts`, "the
  timestamp (ms) that the system generates the data" **[verified 2026-10-09]** — i.e. when
  we learned the bar closed. Two messages for the same bar carry different `ts`, so it can
  never be an identity, but it is the only input to the latency number the README has to
  report. A REST-backfilled bar has no `ts`; it is stored as 0 and excluded from the
  latency statistics.
- **Not used:** `data[].timestamp`, which Bybit documents as "the timestamp of the last
  matched order in the candle" **[verified 2026-10-09]**. That is a property of the last
  trade, not of the bar or of the message, and in a quiet market it can lag the close by
  minutes. It is neither an identity nor a latency reference.

**Caveat to state honestly in the debrief:** `ts` is the exchange's clock and the decision
timestamp is ours, so `ts → decision` includes clock skew and is an *estimate*, not a
measurement. The defensible local number is recv → decision; `ts → recv` is reported
separately and labelled as including skew.

---

## 5. Timeframes and LTF → HTF aggregation

**[done]** — `md_core/htf_aggregator.h`

We subscribe to the **LTF (1h) kline only** and build the **HTF (4h)** locally.

```cpp
enum class TimeFrame : uint8_t { kLtf1h = 0, kHtf4h = 1, kCount = 2 };
```

Bybit interval codes are **`"60"` for 1h and `"240"` for 4h** **[verified 2026-10-09]** —
minutes as a string, not Binance's `1h`/`4h`. The WS topic is `kline.60.{SYMBOL}` with the
symbol **uppercase**, unlike Binance's lowercase stream names.

4h bars align to 00:00 UTC, so the HTF bar containing an LTF bar opens at
`floor(open_time_ms / 4h) * 4h`, and it is **complete** when the contributing LTF bar is
the last of its group: `(open_time_ms / 1h) % 4 == 3`.

The aggregator accumulates: `open` from the first LTF bar, `high`/`low` as running
extremes, `close` from the last, `turnover` as a sum, `event_time_ms` from the LTF bar that
completed it.

On completion it calls **`ApplyCandleComplete(TimeFrame)`** on the symbol, which routes
the finished candle into the HTF consumers. Conceptually a callback; in code a direct call
— no `std::function` on the per-bar path, because an indirect call here buys no
flexibility we need and costs the inliner everything.

**Ordering matters.** An LTF bar that completes an HTF bar must update the HTF side
*before* the filter runs, or the trend test compares a fresh LTF close against a
one-period-stale EMA50 for exactly that bar — once every four hours, which is both wrong
and hard to notice. The order is fixed in §6.

**Rejected:** subscribing `kline.240.{SYMBOL}` instead (the prompt's cut-list option 2).
It removes the aggregator, but doubles the topic count, makes the LTF and HTF closes two
unordered events whose arrival order is not guaranteed, and deletes a component the
exercise list wants built. Keep as the fallback if we run out of time.

**The HTF warm-up history is built the same way** — from LTF bars, by this same
aggregator, not fetched from REST at `interval=240`. One fetch path, and the warm-up HTF
bars are produced by exactly the code that produces the live ones, so the two can never
disagree at a boundary. See §7 for the window size, the alignment requirement and the
byte cost.

---

## 6. Per-symbol state — two timeframe trackers

**[done]** — `md_core/candle_tracker.h`, `md_core/symbol_tracker.h`, `md_core/core.{h,cpp}`

`SymbolTracker` owns one `CandleTracker` per timeframe. Each `CandleTracker` owns the
candle history and the indicators **for its own timeframe**, and nothing else.

### Why the decomposition is by timeframe and not by filter condition

This section previously specified three *signal* managers — `VolumeSurgeManager`,
`TrendManager`, `VolatilityManager` — one per filter condition. That was replaced after
mapping each indicator to the timeframe that drives it:

| indicator | timeframe | read by |
|---|---|---|
| 24-bar turnover windows | LTF 1h | surge |
| Wilder ATR(14) → NATR | LTF 1h | volatility |
| EMA50 | **HTF 4h** | trend |

Every indicator belongs to exactly **one** timeframe. Nothing is fed by both. The
timeframe axis is therefore the axis the *data* already has, and the per-condition axis
was fighting it: the trend condition compares the latest **1h** close against the **4h**
EMA50, so `TrendManager` needed both `UpdateLtf()` and `UpdateHtf()`. A class with two
update paths keyed by timeframe is a class whose decomposition is wrong.

What survives from the old shape is the half that mattered: **`Get()` returns a value,
never a verdict.** The thresholds live in `CoreManager`, so changing one never means
touching an indicator. The three *conditions* are now reads in `CoreManager`:

- surge → `ltf.Turnover()` — two raw sums
- trend → `ltf.LastClose()` vs `htf.GetEma()` — the only one that spans, and it spans as
  two scalar reads
- volatility → `ltf.GetNatrBp()`

Per-timeframe decomposition also makes warm-up and rebuild one call per timeframe, which
is exactly how §7 feeds history back in.

### CandleTracker

```cpp
class CandleTracker {
   public:
    static CandleTracker MakeLtf();   // H1: turnover window + ATR
    static CandleTracker MakeHtf();   // H4: EMA50

    CandleTracker(const CandleTracker&) = delete;
    CandleTracker& operator=(const CandleTracker&) = delete;
    CandleTracker(CandleTracker&&) = default;
    CandleTracker& operator=(CandleTracker&&) = default;

    void Apply(const Candle& c);
    void Reset();

    uint32_t Bars() const noexcept;
    Price    LastClose() const noexcept;

    bool  EmaReady() const noexcept;      Price   GetEma() const noexcept;
    bool  AtrReady() const noexcept;      int32_t GetNatrBp() const noexcept;
    TurnoverWindow<24>::Value Turnover() const noexcept;

   private:
    enum Use : uint8_t { kEma = 1, kAtr = 2, kTurnover = 4 };
    explicit CandleTracker(TimeFrame frame, uint8_t use);

    std::optional<Ema<50>>            ema_;
    std::optional<WilderAtr<14>>      atr_;
    std::optional<TurnoverWindow<24>> turnover_;

    std::deque<Candle> recent_;      // last 10 closed bars, inspection only
    Price    last_close_ = 0;
    uint32_t bar_count_ = 0;
};
```

**`std::optional` members, not `std::vector<std::unique_ptr<IndicatorBase>>`.** The
polymorphic list was tried and rejected. It gives a free `Update()` loop and then destroys
`Get()`:

- the trend condition needs the EMA as a typed `Price` at `kPriceScale`;
- the volatility condition needs NATR as `int32_t` **basis points**;
- the surge condition needs **two** sums.

One `virtual double GetValue()` cannot carry any of those, and Wilder's ATR cannot even
implement `virtual void Update(double)` — True Range needs `high`, `low` *and* the
previous close. The three were never substitutable, so the base class forced each
implementor to lie about its own signature. `std::optional` also costs no heap allocation
and no vtable across 1000 symbols.

**One factory per timeframe**, built on a private `Use` bitmask constructor, so "which
indicators does this timeframe have" is answered in exactly one place.

**`has_value()` and readiness are different questions.** `has_value()` means *this
timeframe uses the indicator at all*; `Ready()` means *it has been fed enough bars to
mean anything*. An EMA50 exists from construction and is meaningless for its first 49
bars, so readiness is a bar counter, and it is `Ready()` that makes a symbol `not ready`.

**Copy deleted, move defaulted.** Deleting the copy is right — duplicating recursive
indicator state gives two trackers that silently drift apart. But declaring *any* copy
operation suppresses the implicit **move** constructor, and without a move `SymbolTracker`
is neither copyable nor movable, which makes `std::vector<SymbolTracker>` fail to compile
on `Cpp17MoveInsertable`: a vector must be able to relocate its elements when it grows.
This cost one build failure to learn.

**`last_close_` is a stored member, not `recent_.back().close`.** `recent_` is empty after
`Reset()` and before the first `Apply()` — and `Reset()` is precisely what a gap rebuild
calls. `std::deque::back()` on an empty deque does not throw, it reads past the end, so
the alternative is a silent garbage read rather than a crash.

### SymbolTracker

```cpp
enum class Arrival { kFirst, kNext, kDuplicate, kStale, kGap };

class SymbolTracker {
   public:
    SymbolTracker(uint32_t id, Symbol symbol);

    Arrival Classify(int64_t open_time_ms) const noexcept;

    // Updates the LTF tracker and, on a 4h boundary, the HTF tracker.
    // Returns the completed HTF candle so the caller knows a boundary passed.
    std::optional<Candle> ApplyLtf(const Candle& c);
    void Reset();

    const CandleTracker& Ltf() const noexcept;
    const CandleTracker& Htf() const noexcept;

    bool IsActive() const noexcept;        void SetActive(bool);
    bool RebuildPending() const noexcept;  void SetRebuildPending(bool);

   private:
    uint32_t id_;
    Symbol   symbol_;        // logging and the active set only
    CandleTracker ltf_;      // by value: both always exist, known size
    CandleTracker htf_;
    HtfAggregator agg_;
    int64_t last_ltf_open_time_ = 0;
    bool is_active_ = false;
    bool rebuild_pending_ = false;
};
```

**Both trackers by value, not `unique_ptr`.** Both always exist and have known size.
Heap-allocating them would cost 2000 allocations across the universe, an indirection on
every bar, and make `SymbolTracker` move-only for no reason.

**No `VenueId`.** One venue. **No `Symbol` inside `CandleTracker`** — it would duplicate
what `SymbolTracker` already holds.

**`SymbolTracker` does not fetch history.** `WarmUp()` doing REST from inside `md_core`
inverts the layering: `ControlManager` owns REST (§2). This class exposes `Reset()` and
`ApplyLtf()`, and whoever holds the history feeds it in oldest-first. One consequence is
that warm-up and gap rebuild are the *same* code path.

**No `warmup_vec_`.** §7 removed it: a bar arriving between the gap and the rebuild is
already contained in the refetch and is dropped by the `open_time` dedup rule, so there is
nothing left to buffer. `rebuild_pending_` is what drops those bars, and it also stops a
second gap from queueing the same symbol twice.

### Dedup state is owned by SymbolTracker, not by an indicator

The indicators never see a bar the identity check rejected. If each tracked its own
`last_open_time`, a bar could be accepted by one and rejected by another, leaving the
symbol permanently inconsistent with no way to detect it. One owner, checked once, before
anything is applied.

### Update order, and the trap in it

```cpp
void CoreManager::ApplyCandle(const Candle& c) {
    if (c.symbol_id >= state_.size()) return;
    SymbolTracker& s = state_[c.symbol_id];

    if (s.RebuildPending()) return;

    switch (s.Classify(c.open_time_ms)) {
        case Arrival::kDuplicate:
        case Arrival::kStale:   return;
        case Arrival::kGap:     RequestRebuild(s); return;
        case Arrival::kFirst:
        case Arrival::kNext:    break;
    }

    const std::optional<Candle> htf = s.ApplyLtf(c);   // BOTH timeframes update here

    ApplyCandleComplete(s, TimeFrame::kH1);
    if (htf) ApplyCandleComplete(s, TimeFrame::kH4);

    Evaluate(s);                                        // ...only then the filter
}
```

**Apply before evaluate.** The bar that closes a 4h candle must update the HTF EMA *before*
the filter runs. Otherwise, on one bar in four, the trend test compares a fresh 1h close
against a one-period-stale EMA50 — wrong once every four hours, which is rare enough to
survive casual testing and frequent enough to matter. `ApplyLtf` updating both sides is
what makes the ordering structural rather than a convention a later edit can break.

### Thresholds live in CoreManager

```cpp
inline constexpr int64_t kSurgeNumerator   = 130;   // recent/prev > 1.30
inline constexpr int64_t kSurgeDenominator = 100;
inline constexpr int32_t kMinNatrBp        = 100;   // 1.00 %
```

```cpp
c.ready      = turnover.ready && ltf.AtrReady() && htf.EmaReady();
c.surge      = (__int128)turnover.recent * kSurgeDenominator
             > (__int128)turnover.prev   * kSurgeNumerator;
c.trend      = ltf.LastClose() > htf.GetEma();
c.volatility = ltf.GetNatrBp() > kMinNatrBp;
```

`Conditions` is returned rather than collapsed to a `bool`, so a transition is logged with
*which* condition failed. "INACTIVE" alone is undiagnosable after the fact, and these
transitions are the product.

### The active set

```cpp
std::unordered_set<Symbol> active_instruments_;
```

Keyed by symbol string, on purpose: it is touched only on a *transition* (at most once per
symbol per hour) and it is read by the printer, which wants the name. String hashing never
reaches the message path, where dispatch is `state_[candle.symbol_id]` — an array index.

**Rejected:** the two-map shape from `shema.md`
(`unordered_map<Symbol, CandleManager> tracked_` + `active_`). Holding `CandleManager`
*by value* in both means two copies of every signal, and "which copy is current" becomes a
question the design has to answer. One owner (`std::vector<SymbolTracker>`, indexed by id)
plus a set of *names* has one answer.

---

## 7. Warm-up, gaps and rebuild

**[done]** — `control_manager/control_manager.cpp`, `md_core/core.cpp`.
This is the core correctness argument of the project.

### Warm-up — one REST call per symbol, LTF only

`GET /v5/market/kline?category=linear&symbol=X&interval=60&limit=603`, once per symbol,
applied oldest-first into `CoreManager::Warmup()`. **The HTF history is not fetched — it
is built from those LTF bars by the same aggregator that builds it live (§5).**

Why 603 and not 48:

| Signal | Needs | In LTF bars |
|---|---|---|
| Volume surge | 48 closed LTF bars (two 24-bar windows) | 48 |
| Volatility | ATR(14) warm-up | 15 |
| Trend | ~150 closed HTF bars for EMA50 | **600** |

So EMA50 sets the window, and 600 LTF bars covers all three. The extra 3 bars are for
alignment (below). Bybit's kline `limit` maxes at 1000 **[verified 2026-10-09]**, so this
is **one request per symbol** — half the request count of fetching LTF and HTF separately,
and the whole universe is ~1000 requests against 600/5 s.

**Why this is better than fetching `interval=240` as well**, reversing the earlier
decision in §5:

- One fetch path instead of two, one parse path, one code path to test.
- The 4h bars used for warm-up are produced by **exactly the code that produces them
  live**. Fetching them from REST instead means warm-up and live HTF bars come from two
  different sources that can disagree at a boundary — a class of bug that simply cannot
  exist now.
- It is fewer requests, not more.

The cost is bytes: ~600 klines/symbol is roughly 60 MB of JSON for the universe, against
~25 MB for the 48+200 split. Warm-up wall time and bytes transferred are both on the
measurement list (§10), because this is the one place where "simpler" costs more of
something.

**Alignment matters.** To build *complete* HTF bars, the oldest LTF bar used must sit on a
4h boundary, i.e. `(open_time_ms / 1h) % 4 == 0`. The leading partial group must be
dropped, or the first aggregated HTF bar is built from 1–3 bars instead of 4 and silently
biases the first EMA value — which then decays away over ~50 bars, making it almost
invisible. Either align the `start` parameter or discard LTF bars until the first boundary.
Named test case in §9.

**Bybit returns klines newest-first** — the docs say the list is "sorted in reverse by
`startTime`" **[verified 2026-10-09]**. The warm-up loop must therefore iterate
`result.list` **in reverse**. Applying them as received would feed the recursive
indicators backwards and make the gap check fire on the second bar, producing EMA and ATR
values that look plausible and are wrong. Named test case in §9.

**No `warmup_buffer`.** Warm-up finishes *before* the WebSocket subscribe, so no live bar
can arrive while a symbol is warming — the buffer the earlier design needed has nothing to
hold. (Phase 1 reintroduces it only if warm-up and the live feed ever overlap, which
happens as soon as REST moves off this thread.)

### Dedup and gap detection

Three cases on arrival, keyed on `open_time` (§4):

| `bar.open_time` vs state | Action |
|---|---|
| `<=` the last applied bar | **drop** — duplicate, or an overlapping backfill. Idempotent. |
| exactly `last + interval` | **apply** |
| `>  last + interval` | **gap**: force INACTIVE, queue a rebuild |

### Why a gap cannot be patched

The indicators are recursive. Once bar `T+1h` has been folded into the ATR state and the
HTF aggregator, there is no way to insert the missing bar `T` afterwards — the state has
moved past it, and a REST backfill that arrives late is unusable as a delta.

So a gap **invalidates the symbol and the symbol is rebuilt from scratch**, reusing the
warm-up path exactly:

1. `CoreManager::ApplyCandle` detects the gap, forces the symbol INACTIVE (so a stale
   symbol never contributes to the output), appends the id to `pending_rebuilds_`, and
   **returns**.
2. The main loop, after the current frame is fully handled, drains `pending_rebuilds_`:
   for each id, reset that symbol's two trackers and aggregator, then run the same
   `Warmup()` call as at startup.
3. Any live bar that closed while the refetch was in flight is handled without a buffer:
   `rebuild_pending_` makes `ApplyCandle` drop it, and the refetch pulls 603 bars, which
   already contain it. Nothing is lost and nothing is applied twice.

   This is why the rebuild path deliberately **drops** rather than buffering, unlike
   startup. The two look symmetric and are not: a rebuild's window is ~364 ms and a full
   603-bar refetch covers it, whereas startup's window is ~18 s and the history fetched at
   its *start* cannot cover a bar that closes near its *end*.

**Why the rebuild is deferred and not inline.** Calling `Warmup()` from inside
`ApplyCandle` would re-enter `ApplyCandle` once per history bar while the outer call is
still on the stack, mutating the state it is in the middle of reading. Recording the id
and returning costs five lines and removes the re-entrancy entirely. This is the one piece
of control flow in the MVP worth reading twice.

This is the same rule as a sequence gap invalidating an order book until resync, and it is
why the warm-up path and the gap path are *one* code path rather than two.

**Rejected:** "dedup by `open_time` makes the backfill idempotent, so a gap needs nothing
else". Idempotence handles *duplicates*; it does not handle a *hole*, because the hole
cannot be filled after the fact.

**Rejected:** keeping enough history to recompute the indicators from any point. That is a
full bar store per symbol and a recompute path only ever exercised by failure — more code,
more state, and the REST refetch is already authoritative.


## 8. Provider — Bybit v5

**[partial]** — `md_provider/md_provider.{h,cpp}` is the consolidated-book provider, kept
for its connection lifecycle and not yet stripped (§11).

### WebSocket

All **[verified 2026-10-09]** unless marked otherwise.

- Endpoint `wss://stream.bybit.com/v5/public/linear`, port 443. (Spot is
  `/v5/public/spot`; we never connect to it.)
- Subscribe frame: `{"op":"subscribe","args":["kline.60.BTCUSDT", ...]}`. The symbol in the
  topic is **uppercase**.
- Push message shape:
  `{"topic":"kline.60.BTCUSDT","type":"snapshot","ts":<ms>,"data":[{"start":<ms>,"end":<ms>,
  "interval":"60","open":"...","high":"...","low":"...","close":"...","volume":"...",
  "turnover":"...","confirm":<bool>,"timestamp":<ms>}]}`.
  In the real frame `ts` precedes `data`, which matters for a simdjson On-Demand parser —
  it reads fields in document order.
- **`confirm == true` means the candle has closed.** This is Bybit's equivalent of
  Binance's `k.x`, and it is the only thing we act on.
- **Push frequency is documented as "1–60s"**, not Binance's ~250 ms. So the
  unconfirmed-candle rate is at most ~1/s/symbol — roughly ≤1000 msg/s for the whole
  universe, against ~1000 *closed* bars per hour. The discard path still dominates the
  message count by ~3600×, but the absolute rate is far below Binance's and the benchmark
  must report the measured rate rather than this bound.
- **The client must send `{"op":"ping"}` every 20 s.** Bybit replies
  `{"success":true,"ret_msg":"pong",...}`. The connection is cut after 10 minutes with no
  ping-pong *and* no data.
  - This is a real inversion from Binance, where the server pings and Beast auto-pongs.
    The provider needs its own 20 s ping timer on the `io_context`.
  - **The ping doubles as the liveness check** (see "Liveness" below), which is why the
    MVP needs no data-silence watchdog at all.
- **No documented forced 24 h disconnect.** The "23 h make-before-break rotation" from the
  Binance plan is therefore dropped, not deferred.
- Connection rate limit: "do not build over 500 connections in 5 minutes", per WebSocket
  domain. One connection plus backoff is far inside it; it only matters if a reconnect
  loop ever goes unbounded, which the backoff prevents.
- **[unverified]** Max args per subscribe *request*: 10 is documented for **spot**, and the
  args array is capped at 21,000 characters for all products. The per-request arg limit for
  `linear` is not stated. Until confirmed, batch **10 topics per subscribe frame** and pace
  them — 1000 symbols is then 100 frames, sent once at startup.
- **[unverified]** Max topics per *connection* for `linear` (2,000 is documented for
  options only). The MVP puts **all symbols on one connection**; ~1000 topics of ~20
  characters is also close to the 21,000-character args cap, which is a second reason the
  subscribe is batched. If one connection cannot hold the universe, `symbols_per_conn`
  in config splits it — see below. **This must be checked empirically on the first run**;
  the docs do not answer it.

### Liveness and reconnect

**There is no data-silence watchdog.** It was in the earlier design and is deliberately
gone, for two reasons:

1. **One connection carries every symbol**, so there is nothing per-symbol to watch. A
   symbol going quiet is a quiet market; the *connection* is the only thing whose health
   is a yes/no question.
2. **The threshold would have been a guess.** It rested on the documented push frequency
   topping out at 60 s, which the docs state as a range ("1–60s") and not a guarantee. A
   backstop set too short marks a healthy quiet feed dead and flaps it; that is worse
   than having none.

What replaces it costs almost nothing, because the ping is mandatory anyway:

- **Missed-pong check.** We must send `{"op":"ping"}` every 20 s. Bybit answers every one.
  If N consecutive pings go unanswered (N = 3, i.e. ~60 s), the connection is dead —
  reconnect. This is a *protocol-level* fact, not a guess about market activity, and it
  catches the case Beast cannot: a half-open socket where no bytes arrive and no error is
  ever reported.
- **Beast's error path.** A read error, EOF or TLS failure fires the session's on-closed
  callback and reconnects with exponential backoff 1 s → 60 s, ±20 % jitter.

The two together cover both failure modes: detected death (Beast) and silent death
(missed pong). Neither needs a number nobody can defend.

- On reconnect, the full subscribe batch is re-sent, and every symbol's next bar will
  either continue cleanly or trip the gap rule and rebuild (§7). Nothing special is needed.
- **Partition:** the MVP uses one connection. `symbols_per_conn` exists in config so the
  universe can be split if the unverified per-connection topic cap turns out to bite, but
  K > 1 connections on one `io_context` is still **one thread** — it does not bring back
  the queue.
- Hot add/remove of newly listed symbols is v2.
- **Record/replay:** a provider can dump raw frames with a receive timestamp to JSONL, and
  a replay source feeds that file through the same parser. This is what makes the tests
  deterministic, the demo possible without waiting for an hour boundary, and the parse
  benchmark reproducible.

### The cheap discard — Phase 1, item 2

The confirmed-bar rate is ~1000/hour; everything else is an unconfirmed candle update.
**The MVP parses every frame and checks `confirm`** — that is the baseline. Phase 1
replaces it with a raw-frame scan:

```cpp
if (frame.find("\"confirm\":true") == std::string_view::npos) return;   // no parse
```

This is the one item in §13 that is near-certain to matter, because it is the only part
of the system that runs thousands of times per second rather than ~1000 times per hour.
It still has to be measured, including the unconfirmed message rate itself.

This is a **prefilter, not the decision**. `data` is an array, so a frame could in
principle carry a confirmed and an unconfirmed candle together; the parser therefore still
checks `confirm` per element. The scan only rejects frames that cannot contain a closed
bar — which is almost all of them. A replay test pins the literal, including that the
subscribe ack (`"success":true`) and the pong reply do not match it.

### REST

**[partial]** — `md_provider/rest.{h,cpp}` is a working blocking HTTPS GET; it returns the
body only and must be extended to expose status and headers. The limiter does not exist.

All **[verified 2026-10-09]**:

- Host `api.bybit.com`, port 443.
- **Symbol universe:** `GET /v5/market/instruments-info?category=linear`, filtered to
  `contractType == "LinearPerpetual"`, `quoteCoin == "USDT"`, `status == "Trading"`.
  Default page size is 500 and **there are more than 500 linear symbols**, so
  `limit=1000` plus **`cursor` pagination** is mandatory — a single default request
  silently truncates the universe.
- **History:** `GET /v5/market/kline?category=linear&symbol=&interval=60|240&limit=&start=`.
  `limit` range is [1, 1000], default 200. `result.list` is an **array of arrays of
  strings**: `[startTime, open, high, low, close, volume, turnover]`. For USDT contracts
  `turnover` is the quote-coin amount and `volume` is base — **we want `turnover`**,
  index 6. Sorted **newest-first** (§7).
- **Rate limit: 600 requests per 5 s per IP** on `api.bybit.com`. Exceeding it returns
  `403 access too frequent` / `retCode 10006 "Too many visits!"`, and the ban lifts
  automatically after at least 10 minutes.
  - At 2 requests per symbol, the full 1000-symbol warm-up is ~2000 requests — roughly
    **17 s of wall time at the documented ceiling**, and well under a minute at a
    self-imposed fraction of it. This is far more generous than Binance's weight budget;
    warm-up is not the bottleneck it would have been there. *(Arithmetic from the
    documented limit, not a measurement.)*
  - The limiter **self-paces against 600/5 s** rather than re-syncing from a header. Bybit
    does publish `X-Bapi-Limit`, `X-Bapi-Limit-Status` and
    `X-Bapi-Limit-Reset-Timestamp`, but those describe the **per-UID endpoint quota**, and
    the limit that applies to our unauthenticated market calls is the **IP** one, which
    those headers do not report. **[unverified]** whether they appear at all on public
    endpoints. Read them if present, but never depend on them — this is the one place the
    Binance design (trust `X-MBX-USED-WEIGHT-1M`) does not carry over.
  - Backoff on 403/10006 must be at least 10 minutes, since that is the stated unban
    delay; retrying sooner just extends the ban.
- REST is blocking and must never run on a provider's `io_context` thread: that thread
  drives the WebSocket reads.

---

## 9. Testing

**[partial]** — 73 tests pass under both the plain debug and the ASan+UBSan builds
(`test_indicators`, `test_htf_aggregator`, `test_core`, `test_bybit_parser`,
`test_spsc_queue`). What is still missing is listed at the end of this section.
`tests/unit_tests/src/test_spsc_queue.cpp` is reusable
as-is.

Per the working mode, every DRILL component ships with its own unit tests, written
alongside it:

- `SpscQueue` (Phase 1, when the queue is actually used): single-threaded semantics,
  wrap-around, full/empty edges, plus a **TSan stress test** of ≥10 M ops across 2 threads
  with a checksum. The existing `test_spsc_queue.cpp` already covers the single-threaded
  part and keeps passing meanwhile.
- Padding benchmark (Phase 1): padded vs unpadded head/tail throughput.
- `FixedHashMap` (Phase 1): collisions, load factor behaviour, no allocation after
  construction.
- `FixedRing` (Phase 1): wrap, O(1) rolling sums against a naive sum.
- Indicators: EMA, Wilder ATR/NATR and the HTF aggregator, each against a naive
  **floating-point** reference — with an explicit tolerance derived from the §3 error
  budget (~13 units of `kPriceScale`), not `1e-9`. The integer versions are deliberately
  not bit-identical to a double reference, and a test that pretends otherwise would fail
  for the wrong reason. Also test the two extremes directly: BTC-scale prices and a
  0.000006-scale price, since precision is the whole argument for `kPriceScale = 1e10`.
- Overflow: a BTC-scale ATR through the NATR expression, asserting the `__int128` path is
  used and the int64 one would have wrapped.
- Each indicator in isolation: `Get()` before readiness, the readiness boundary
  (bar 47 vs 48, HTF bar 149 vs 150, ATR bar 14 vs 15), and the computed value against a
  hand-worked example.
- CoreManager: dedup, the three `open_time` cases, HTF aggregation boundaries, **the
  apply-before-evaluate ordering** (a bar that completes an HTF candle must not be
  evaluated against the old EMA), and ACTIVE/INACTIVE transition edges.
- **Deferred rebuild:** a gap must queue the symbol and return, not rebuild inline. Assert
  that `ApplyCandle` is not re-entered while it is on the stack, that the symbol is forced
  INACTIVE immediately, and that a bar arriving after the refetch with an already-covered
  `open_time` is dropped rather than applied twice (§7).
- **HTF warm-up alignment:** an LTF fixture whose oldest bar is *not* on a 4h boundary must
  discard the leading partial group. Compare the resulting EMA against the same fixture
  trimmed to a boundary — they must match, which is what proves the partial group was
  dropped rather than aggregated.
- **Kline ordering:** a REST fixture in Bybit's documented newest-first order must produce
  the same state as the same bars applied oldest-first. This is the test that catches the
  §7 reversal bug.
- Parser: `confirm=false` rejected; a frame with mixed confirmed/unconfirmed elements in
  `data` handled per element; subscribe ack and pong reply not mistaken for a candle.
  (Phase 1 adds: the raw-string prefilter agrees with the parser on every fixture.)
- Replay: a recorded `confirm=true` burst replays deterministically.
- **No-alloc test (Phase 1):** a malloc-counting hook proves the frame → parse → core
  path allocates zero times after startup.
- Fault injection: killing the socket recovers, and every symbol either continues cleanly
  or rebuilds — no symbol is left stale and ACTIVE.
- **Missed-pong liveness:** N unanswered pings must trigger a reconnect, and a pong must
  reset the counter. A silent half-open socket is the case this exists for, so the test
  drives it directly rather than relying on a real network failure.

Builds: `-Wall -Wextra -Werror`, debug with ASan+UBSan, and a separate TSan build for the
concurrency tests. Presets exist in `CMakePresets.json`.

---

## 10. Measurements to report

No numbers yet. **Nothing in this document is a measurement.** The README must report,
with the method stated:

- parse ns/message (confirmed bar, and the discarded case separately);
- the measured unconfirmed-candle message rate, against the ≤1000/s bound in §8;
- SPSC ops/s, padded vs unpadded (Phase 1 microbenchmark — the MVP has no queue);
- p99 `ts` → decision, with the clock-skew caveat from §4;
- allocation count on the hot path (MVP baseline, then 0 after Phase 1 item 8);
- per-bar update ns, MVP vs each Phase 1 change (§13).

Every number is reported as a before/after pair with the change that caused it. A Phase 1
item that produced no measurable change is reported as such, not quietly dropped.

---

## 11. Reuse inventory

The repository began as a copy of the consolidated-book project. What was kept, and in
what state:

| Path | State | Note |
|---|---|---|
| `md_provider/ws/` | **[done]** | Beast WSS session: connect, TLS/WS handshake, optional subscribe frame, read loop, once-only on-closed callback. Compiles as-is. Needs a ping timer added for Bybit (§8). |
| `md_provider/rest.{h,cpp}` | **[done]** | Blocking HTTPS GET. Must be extended to return status and headers, and wrapped in the 600/5 s limiter. |
| `md_provider/base_parser.h` | **[done]** | Reused simdjson parser + growable input buffer, so a steady stream does no per-message allocation. |
| `md_provider/decimal.h` | **[partial]** | `ParseScaledDecimal<Scale>`: decimal string → scaled integer, no floating point. Used for every price and turnover. Needs `kMultipliers` extended past 1e8 to 1e10 and the `Scale <= 8` static_assert relaxed (§3). |
| `md_core/spsc_queue.h` | **[done, unused in the MVP]** | Padded acquire/release SPSC ring. The MVP is single-threaded and uses a callback instead (§2), so nothing links it yet. It stays in the tree because Phase 1 needs it the moment parsing moves to its own thread, and because rewriting it cold is a DRILL. |
| `logger/logger.h` | **[done]** | fmt-based, level-gated before formatting. |
| `tests/unit_tests/src/test_spsc_queue.cpp` | **[done]** | Venue-agnostic; applies unchanged. |
| `md_provider/md_provider.{h,cpp}` | **[partial]** | Keeps: reconnect with backoff, the timer plumbing (repurposed for the 20 s ping), `OnReconnect` hook. Must drop: its **own thread and io_context** (the MVP shares the main one), the depth/BBO two-stream split, `SeqDedup`, `RequestResync`, the multi-state venue-health machine, redundant connections, `PostToIoContext` (nothing to marshal across), and `ProviderConfig`'s venue/market/depth-tier fields. Does not compile until stripped (dangling includes). |
| `config/config.{h,cpp}` | **[partial]** | simdjson DOM parse helpers worth keeping; the content (venues, depth tiers, markets) is all consolidated-book. Does not compile until rewritten. |
| `types/candle.h` | **broken** | Python pasted into a header and then clang-formatted. To be replaced by the `Candle` of §4 — **DRILL #11, Anton writes it.** |

The old `md_provider/bybit/` provider and parser were deleted with the rest of the
order-book code, but they are the source of the verified endpoint and subscribe-frame
details in §8, and they are still in the backup tarball if the framing needs rechecking.

Deleted: `aggregator/`, `client/`, `md_core/consolidated_*`, `flat_order_book`,
`md_proto/`, `md_wire/`, `md_provider/{binance,bybit,okx}/`, `continuity`, `seq_dedup`,
`types/{venue,venue_registry,instrument_registry}.h`, `user_config/`, `benchmarks/`,
20 consolidated-book tests, Docker files, and the old `README.md`/`QuickStart.md`.
A tarball of the tree as it stood before the clearing is at
`~/Documents/screener_prescreener_backup_2026-10-09.tar.gz`.

### Target layout

```
screener/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  config.json
  types/            candle.h  timeframe.h
  config/           config.{h,cpp}
  logger/           logger.h
  md_core/          spsc_queue.h  fixed_hash_map.h  fixed_ring.h
                    core_manager.{h,cpp}  symbol_state.h  filter.h
                    htf_aggregator.h
                    volume_surge_manager.h  trend_manager.h  volatility_manager.h
                    indicators/  indicator.h (CRTP base)  ema.h  atr.h
  md_provider/      md_provider.{h,cpp}  kline_parser.{h,cpp}  rest.{h,cpp}
                    rate_limiter.h  base_parser.h  decimal.h  replay.{h,cpp}
                    ws/
  control_manager/  control_manager.{h,cpp}  main.cpp
  tests/unit_tests/
```

---

## 12. Open questions

1. ~~Numerics~~ — **decided:** scaled integers, `kPriceScale = 1e10`,
   `kVolumeScale = 1e6`, no floating point (§3).
2. **`SpscQueue`** — rewrite cold as DRILL #13, or reuse the existing one and drill only
   the TSan stress test and the padding benchmark?
3. **Bybit WS limits (§8)** — partly answered. **Measured 2026-10-09:** 791 topics at
   `topics_per_subscribe = 100` → 8 frames, all 8 acked `success:true` on one connection,
   8018 frames received in 100 s, 0 reconnects. So one connection holds the whole universe
   and 100 args per frame is accepted. Still open: where the actual ceiling is — the
   21,000-character args cap is `[unverified]` for `linear` (§8), and `config.h`'s comment
   states it as fact, which overstates what we know. Worth one run at
   `--topics-per-sub=800`. (The "is 1–60 s a guarantee?" question is gone: nothing depends
   on it now that the data-silence watchdog is replaced by the missed-pong check.)
4. **Transition output** — console table only, or also a JSONL event log? The prompt's
   definition of done requires the table and "logs transitions"; a structured log is cheap
   and makes the demo reproducible.
5. **The startup buffer's replay path has never run** (§2). Both live runs replayed 0
   candles because no hour boundary fell inside the 18 s warm-up, which is the expected
   0.5 %. It needs either a `ControlManager` test with an injected REST source, or a run
   started deliberately ~20 s before the hour. Until then the reconciliation is argued,
   not demonstrated.
6. **A failed rebuild is permanent.** `CoreManager::Warmup` is what clears
   `rebuild_pending_`, so if the refetch fails the flag stays set, every subsequent bar for
   that symbol is dropped, and `RequestRebuild` early-returns so nothing re-queues it. The
   symbol is INACTIVE forever. Fail-safe but not self-healing; needs a
   `CoreManager::ClearRebuildPending` plus a retry with backoff.
7. **`screener_prompt.md` is still written against Binance** — the filter, the roadmap and
   the exercise list all still apply unchanged, but every API fact in its §2 is wrong for
   this project. Either annotate it or treat §8 of this file as the only venue reference.

---

## 13. Delivery plan — MVP first, then measured optimisation

The project is built in two phases, and the phase boundary is deliberate.

**Phase 0 builds the simplest correct screener that runs end to end.** Ordinary
containers, no layout tricks, no custom data structures. It must be *correct* and
*measurable*, not fast.

**Phase 1 optimises it, one change at a time, each gated by a benchmark.** This is where
the interview-prep exercises live. The rule for every item:

1. State the suspected bottleneck.
2. **Benchmark the MVP baseline** and record the number.
3. Make the smallest useful change.
4. **Benchmark again** and record the delta.
5. Verify correctness (the Phase 0 test suite must still pass) and tail latency.
6. Write down the result — **including when it is "no measurable difference"**.

### Why this order

Three reasons, and they are all defensible in a debrief:

- **Nothing can be justified without a baseline.** "I used an open-addressing hash map
  because it is faster" is an assertion. "I replaced `unordered_map` and parse time went
  from X to Y, while the map lookup turned out to be 0.3 % of the path, so I reverted it"
  is engineering.
- **A screener at ~1000 updates/hour is the wrong system to be fast.** Several of the
  items below will show no gain, and discovering *which* ones is a far better answer to
  "where would kernel bypass help, and where is it pointless?" than optimising everything
  and claiming it all mattered.
- **An MVP that works is a demo.** An optimised system that does not run is nothing.

### Phase 0 — MVP

Correctness features only. All of these are needed for the screener to be a screener:

| Area | MVP form |
|---|---|
| Threading | **one thread, one `io_context`** — no queues, no joins, no atomics (§2) |
| Provider → core | **direct callback** into `CoreManager::ApplyCandle` |
| Symbol universe | `instruments-info` with cursor pagination, filtered to Trading/USDT/LinearPerpetual |
| Symbol → id | `std::unordered_map<std::string, uint32_t>`, built once at startup |
| Symbol state | `std::vector<SymbolTracker>`, indexed by id |
| Candle history | `std::deque<Candle>` / `std::deque<Volume>` |
| Candle | natural layout, `int64_t` fields, no size assert |
| Indicators | plain classes, no CRTP, integer math per §3 |
| Warm-up | **async, 16 concurrent, one REST call per symbol, `interval=60` only**; HTF aggregated locally (§7) |
| Startup race | subscribe first, buffer into `startup_buffer_`, replay on the last warm-up response (§2) |
| Connections | **one** WebSocket for the whole universe, batched subscribe |
| Liveness | missed-pong check on the mandatory 20 s ping; no data-silence watchdog |
| Rebuild | deferred to the main loop, **async** REST, reuses the warm-up path |
| Discard | parse the frame, check `confirm` — **no raw-string prefilter yet** |
| REST pacing | self-paced against 600/5 s; one sliding window, `Acquire()` blocking + `AsyncAcquire()` on the loop |
| Output | console table + transition log lines |
| Tests | the full correctness suite from §9 (this is not an optimisation) |

What the MVP deliberately does **not** have, with the reason it is safe to omit:

| Omitted | Why it is safe now | Comes back when |
|---|---|---|
| SPSC queues | one thread — producer and consumer are the same thread | parsing moves to its own thread |
| Provider threads | ~1000 msg/s at ≤5 µs each is 0.5 % of a core | a measurement says parse is the bottleneck |
| Restart/one-producer proof | nothing to prove without threads | the queue comes back |
| `warmup_buffer` | warm-up completes before the subscribe, so no live bar overlaps it | REST moves off this thread |
| Data-silence watchdog | one connection for all symbols; the threshold would be a guess | never — the missed-pong check is strictly better |
| HTF REST fetch | the live aggregator builds it, so the two cannot disagree | never |
| ~~Non-blocking REST~~ | **done** — warm-up and rebuild are async; only `instruments-info` still blocks, before `ioc.run()` | n/a |

Phase 0 is **done** when: the screener runs live against Bybit, warm-up completes for the
full universe, closed bars arrive and move symbols between ACTIVE and INACTIVE, a killed
socket recovers without losing a bar, replay is deterministic, and every correctness test
passes under ASan/UBSan.

### Phase 1 — the optimisation backlog

Ordered by dependency, not by expected payoff. Each is a DRILL: Anton writes it, with a
benchmark before and after.

| # | Change | Baseline to measure first | Expected to matter? |
|---|---|---|---|
| 1 | **Benchmark harness** (warm-up, median + p99, `DoNotOptimize`) | — it is the prerequisite for everything below | n/a |
| 2 | Raw-string `"confirm":true` prefilter instead of parsing | ns/message on unconfirmed frames, and their measured rate | **yes** — this is the only true hot path |
| 3 | `FixedRing` instead of `std::deque` | per-bar update ns; allocation count | maybe — bars are hourly |
| 4 | `Candle` at exactly 64 bytes + `static_assert` | queue push/pop ns; cache misses | unclear — the honest answer may be "no" |
| 5 | `FixedHashMap` instead of `unordered_map` | symbol-lookup share of the parse path | **probably not** — lookups are ~1000/hour, and saying so with a number is the result |
| 6 | SPSC head/tail padding, padded vs unpadded | queue throughput, 2 threads | yes, as a microbenchmark; irrelevant at this message rate |
| 7 | CRTP indicators instead of virtual | per-bar update ns; inspect the asm | yes in the microbenchmark |
| 8 | Zero-allocation hot path | malloc count via a counting hook | it is a *correctness* property for an HFT claim, measured not argued |
| 9 | `SpscQueue` rewritten cold as the DRILL exercise | existing queue as the reference to diff against | n/a — practice, not performance |
| 10 | **Parsing on its own thread**, callback → SPSC push | parse share of the io_context thread; p99 under an hour-boundary burst | only if item 2 is not enough. This is the item that restores the restart/one-producer argument, and it must not be done before a measurement justifies it |

Items 3–7 are exactly the structures `screener_prompt.md` §4 lists as DRILLs. Moving them
behind a benchmark does not drop them — it means each one arrives with a number attached,
which is what the prompt's §7 definition of done actually asks for.

### What goes in the README

For each Phase 1 item: the baseline, the change, the new number, and the verdict. A table
with "no measurable change" in it is a stronger artefact than one where every row is a
win, because it shows the measurements were real.

---

## 14. Telegram notifications — **[partial]**

A reporting sink, not a decision path. `notifier/telegram_notifier.{h,cpp}` sends three
messages to one chat: the screener started, a symbol became ACTIVE, a symbol left ACTIVE.
It is off unless `--telegram` is passed, and a run without it behaves exactly as it did
before the class existed.

Not in `screener_prompt.md`. Classified **[SCAFFOLD]** — it is I/O plumbing, like the REST
client and the rate limiter — except the `CoreManager` hook, which is **[DRILL]** (§14.4).

### 14.1 Transport — reuse `AsyncHttpsGet`

Telegram's `sendMessage` accepts its parameters in the query string, so there is no POST
transport to write: a send is one `AsyncHttpsGet` to `api.telegram.org`, on the same
`io_context` and the same `ssl::context` as everything else. Nothing here blocks the
WebSocket read loop.

Two consequences worth stating:

- **The bot token is in the URL path** (`/bot<token>/sendMessage`). `AsyncHttpsGet` logs
  `host + target` on every failure, so `async_rest` grew an optional `log_target`
  parameter that replaces the real one in log lines. Without it, one DNS blip writes a
  live credential into the log. Market-data callers pass nothing and are unaffected.
- **No `parse_mode`.** The text is plain UTF-8, so there is no Markdown or HTML escaping
  to get wrong — and a symbol name is exactly the kind of string that eventually contains
  an underscore and turns a notification into a 400 nobody reads.

`notifier` links `md_provider` to reach `AsyncHttpsGet`, which is a wart: a chat notifier
has no business depending on a market-data provider. The clean fix is to lift
`async_rest.{h,cpp}` into its own `net` library. Deliberately not done — it is churn
across four `CMakeLists.txt` for no behaviour change.

### 14.2 Pacing, and why there is a queue

Transitions arrive in **bursts**. The filter runs on a closed 1h bar, so at the top of the
hour dozens of symbols can flip within milliseconds of each other, while Telegram's
per-chat limit is around 20 messages per minute. Sending straight from the transition
would earn a 429 exactly when the screener had something to say.

So messages go into a FIFO drained by a `steady_timer`, one every
`telegram_min_send_interval_ms` (default 3000 — a deliberate ~20/minute ceiling, not a
guess). The first message in an idle period goes out immediately; the timer only paces the
ones behind it, because a lone transition should not wait an interval for nothing.

The queue is capped at `telegram_max_queue` (default 100, ~5 minutes of backlog). Past the
cap the **new** message is dropped, not the oldest — the queue holds transitions in the
order they happened, and discarding the head would report the chat's history out of order,
which is harder to read than a gap in it. Drops are counted and reported into the chat
once the queue drains, because the operator this class exists for is not reading the log.

Its own timer, never the Bybit `RateLimiter`: different host, different limit, and one
shared window would let chat traffic eat the kline request budget.

### 14.3 Message shape

`parse_mode=HTML`, one bold symbol per line:

```
🚀 <b>Screener started</b>
Tracking 791 instruments: 781 ready, 10 not ready

<b>Not ready (10):</b>
AUSDT - no history (warm-up request failed)
NEWUSDT - 12 x 1h, 3 x 4h (EMA50 needs 50 x 4h)
...

<b>Active (39):</b>
<b>AMZUUSDT</b>
<b>API3USDT</b>
...
```

**HTML and not MarkdownV2.** Bold needs a `parse_mode`, and MarkdownV2 requires 18
characters escaped (``_*[]()~`>#+-=|{}.!``) against HTML's three (`&<>`). One missed
character is a 400 on a notification nobody sees fail, so the smaller escape surface wins.
Every dynamic value goes through `HtmlEscape` before it reaches a message.

**The ready gap is spelled out, not left as a subtraction.** "791 tracked, 781 ready" is
ten instruments that silently dropped out of the product, and the two causes behind it are
not the same problem: a new listing genuinely has no history, while `ltf=0` means its
warm-up *request* failed and the symbol is missing for a reason we could fix. So every
not-ready symbol is named with its bar counts, or with "warm-up request failed".

**The active list is complete by default** (`telegram_active_list_max = 0`). A truncated
list hides exactly the symbol the operator went looking for. Length is handled by
`Enqueue`, which splits an oversized message **on line boundaries** into several — line
boundaries because a cut inside `<b>...</b>` is a 400 on both halves. The knob stays for
the other problem: a few hundred ACTIVE symbols means every transition re-sends a list two
messages long, and capping that is a chat-noise decision rather than a protocol limit.

One symbol per line because these are read on a phone, where a comma-separated run of 39
tickers is a wall.

The startup message also carries the symbols that came out of warm-up **already** ACTIVE.
Those are not transitions, so no ACTIVE message is ever sent for them, and without this the
chat would never learn about them.


### 14.4 Where the hooks are — and the one that is [DRILL]

`NotifyStarted` is wired in `ControlManager::FinishWarmup`, after the replay and after
`LogWarmupReadiness` (which now returns the ready count). Not from `Run()`: before warm-up
every symbol is `not ready`, so an earlier message would report a count the screener cannot
act on yet.

The ACTIVE/INACTIVE hook is **[not implemented]** and is a DRILL for Anton:

```cpp
struct Transition { uint32_t symbol_id; bool active; };
std::vector<Transition> TakeTransitions();   // swap-out, like TakePendingRebuilds()
```

- `TryActivate` pushes `{id, true}`, `TryDeactivate` pushes `{id, false}`, edge-triggered.
- **`RequestRebuild` must also push `{id, false}`** — it forces INACTIVE inline and
  bypasses `TryDeactivate`. Miss it and the chat keeps showing a symbol as active after a
  gap has invalidated it.
- `SetUniverse` clears the list.

`ControlManager` drains it in `DrainRebuilds`, next to the rebuild drain, which is already
`post`ed and therefore already runs after `ApplyCandle` has returned. A direct
`std::function` call from inside `TryActivate` was rejected for two reasons: it would put
chat formatting and a URL-encode on the message path, and it would give `md_core` a
dependency on network I/O. Until the hook lands, transitions reach the log only.

### 14.5 Credentials — `.env`, then the environment

`TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` are never flags: a token in `argv` is readable
by any user via `ps` and lands in shell history. That is the one reason this project reads
anything other than `argv` — secrets are the exception to "every tunable is a flag", not a
second configuration mechanism.

Two sources, read in this order:

1. **`.env`** in the working directory, `--env-file=PATH` to move it. Gitignored, with a
   checked-in `.env.example`. A missing default `.env` is fine — that is the CI and
   container case, where the variables come from the environment. A missing file the
   operator *named* is an error, because they said where to look.
2. **The process environment**, which **wins**. The file supplies defaults so a normal run
   needs no exports; an explicit export still redirects one run to a test chat without
   editing the file.

`ApplyDotEnv` is deliberately not a dotenv implementation: no interpolation, no multi-line
values, no `.env.local` layering. `KEY=VALUE` per line, `#` comments (whole-line **and
inline**), blank lines, an optional `export ` prefix, optional surrounding quotes (which is
how a `chat_id` starting with `-` gets written without looking like a flag).

**The inline comment is the part that bit us.** `TELEGRAM_BOT_TOKEN = "123:AAA"  # from
@BotFather` is what a hand-written `.env` looks like, and the first version only stripped
quotes when the first *and last* character of the value were matching quotes — which the
trailing comment prevents. The token therefore kept its quotes *and* the comment, went into
the request path spaces and all, and Telegram answered **400 from its HTTP parser**, before
the Bot API saw anything. The fix is `ExtractValue`: a quoted value ends at its closing
quote and only a comment may follow; an unquoted value ends at the first `#`.

Two defences, because a secret that is silently wrong is expensive to diagnose:

- `Validate()` rejects a token or `chat_id` containing a space, control character, quote or
  `#`. Both go into the request line, so those characters cannot produce anything but a
  malformed request — refusing at startup names the cause, a 400 an hour into a run does
  not.
- `AsyncHttpsGet` now logs the response **body** on a non-200, truncated to 256 bytes. Both
  services explain a 4xx in it (Bybit in `retMsg`, Telegram in `description`), and without
  it a status code alone is unexplainable.

Two more rules worth stating:

- **Unknown keys are ignored.** A `.env` is shared with whatever else runs in the
  directory, so failing on another tool's `DATABASE_URL` would be this process
  overreaching.
- **A malformed line, or an empty value for a key we own, is fatal** — the same strictness
  as an unrecognised flag. A silently skipped `TELEGRAM_CHAT_ID` line would surface much
  later as "needs TELEGRAM_CHAT_ID" with the variable sitting right there in the file.

A `.env` readable by other users is **warned** about, not rejected: a 644 file on a dev
laptop is normal, and refusing to start over it is the wrong trade.

Neither source enables notifications; `--telegram` does. `--telegram` with either secret
missing is a **validation failure**, not a silent no-op: honouring the flag by doing
nothing is exactly the failure nobody notices until the first ACTIVE never arrives.

### 14.6 Testing

`tests/unit_tests/src/test_config.cpp`, 12 tests: the dotenv parser on *content*, because a
parser's failure modes are its lines and not its inode — comments, whitespace, quotes, an
`export` prefix, CRLF, a missing final newline, unknown keys, and each fatal case. Four go
through `FromArgs` with a temporary file, for the properties only the read ORDER can show:
the shell beating the file, an explicitly named missing path being fatal, `--telegram`
without secrets being refused, and secrets without `--telegram` leaving notifications off.

`tests/unit_tests/src/test_telegram_notifier.cpp`, 13 tests. Twelve inject a `Sender` and
use no network and no token: what is worth testing is not "does an HTTPS GET work" but the
message format, the HTML escaping, the sort, the burst order, the drop policy and its
report, that a disabled notifier is inert, and the split: a 1000-symbol list must come out
as several messages, each under 4096 characters, each with balanced `<b>` tags, with no
symbol lost across the cut — all pure functions of the queue.

One uses the **real** transport, because it is the only way to check what the
transport *logs*: it points `telegram_host` at a name in `.invalid` (RFC 2606 reserves it
as never-resolvable, so the test fails at DNS and never opens a socket), captures stdout,
and asserts the token does not appear and `<redacted>` does. That is the one regression
worth a test here — dropping the `log_target` argument leaks a live credential, and
nothing else would catch it.

A successful send is **not** unit-tested — faking Telegram's server would test the fake.
It has a `DISABLED_` manual smoke test instead, `TelegramNotifierManual.SendsHelloToTheRealChat`:

```
./unit_tests --gtest_also_run_disabled_tests --gtest_filter='*SendsHello*'
```

It reads the real `.env` (via `SCREENER_SOURCE_DIR`, so it works from the build tree),
sends a realistic startup message through the real transport, and prints what came back. It asserts nothing about the send: the outcome depends on a bot and a `chat_id` this
process cannot verify, so what it gives you is Telegram's own response body. `GTEST_SKIP`
when there is no usable `.env`, and `DISABLED_` so a normal run never touches the network.
