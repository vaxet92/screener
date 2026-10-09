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
    1. REST instruments-info (cursor paged) -> symbol universe -> ids
    2. REST kline interval=60 per symbol -> CoreManager::Warmup()   [LTF ONLY, see 7]
    3. WS connect + batched subscribe                     [md_provider/]
    4. ioc.run():
         WS frame -> parse -> Candle -> CoreManager::ApplyCandle()   <-- direct callback
         20s ping timer -> send {"op":"ping"}, check pong freshness
         after each frame -> drain pending rebuilds (blocking REST)

  CoreManager                                             [md_core/]
    dedup/gap by open_time -> three signal managers -> HTF aggregate
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

The MVP calls REST synchronously on the only thread, which stalls WebSocket reads for the
duration. This is acceptable, and the reason is worth being precise about:

- **Warm-up** runs *before* the WS subscribe, so there is nothing to stall.
- **A rebuild** is ~1 request, a few hundred ms. Inbound frames queue in the kernel and
  Beast's buffer and are read afterwards; nothing is lost. The 20 s ping timer is also
  delayed, which is harmless against Bybit's 10-minute idle cutoff.
- It also *removes* a problem: no live bars are processed during a rebuild, so there is no
  interleaving to reason about (§7).

**Known MVP limitation:** a rebuild storm — many symbols gapping at once — serialises into
one long stall. Phase 1's fix is a separate REST thread handing results back through a
queue, which is the same migration as the callback → SPSC one.

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

**[not implemented]** — MVP form below; the 64-byte layout is a **Phase 1** exercise
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

**[not implemented]**

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

## 6. Per-symbol state — three signal managers

**[not implemented]**

Each filter condition is one class that owns its own logic and its own storage, and
exposes exactly two operations: `Update()` and `Get()`.

Shown in **MVP form**: plain `std::deque` / `std::vector` history, no fixed-capacity
containers. Swapping those for `FixedRing` is a Phase 1 exercise with a benchmark on
either side (§13); the interfaces below do not change when it happens, which is the
point of writing them this way first.

```cpp
// Volume surge: two adjacent 24-bar windows over LTF turnover.
class VolumeSurgeManager {
    std::deque<Volume> turnover_;           // MVP; FixedRing<Volume, 64> in Phase 1
    Volume   recent_sum_ = 0, prev_sum_ = 0;
    uint32_t bars_ = 0;
  public:
    void Update(const Candle& ltf);
    struct Value { bool ready; Volume recent; Volume prev; };   // raw sums, not a ratio
    Value Get() const;
};

// Trend: EMA50 on HTF, compared against the latest LTF close.
class TrendManager {
    Ema<50>  ema_htf_;                      // int64 state at kPriceScale
    Price    last_close_ltf_ = 0;
    uint32_t htf_bars_ = 0;
  public:
    void UpdateLtf(const Candle&);          // records the close to compare
    void UpdateHtf(const Candle&);          // advances EMA50
    struct Value { bool ready; Price close; Price ema; };
    Value Get() const;
};

// Volatility: Wilder ATR(14) on LTF, reported in BASIS POINTS.
class VolatilityManager {
    WilderAtr<14> atr_;                     // int64 state at kPriceScale
    Price    prev_close_ = 0, last_close_ = 0;
    uint32_t bars_ = 0;
  public:
    void Update(const Candle& ltf);
    struct Value { bool ready; int32_t natr_bp; };   // 100 bp = 1.00 %
    Value Get() const;
};

struct SymbolState {
    uint32_t id;
    Symbol   symbol;                   // std::string: logging and the active set only

    VolumeSurgeManager volume_surge;
    TrendManager       trend;
    VolatilityManager  volatility;

    HtfAggregator      agg;            // 4 x LTF -> 1 HTF
    std::deque<Candle> recent_ltf;     // last N closed candles, inspection only
    std::deque<Candle> recent_htf;

    int64_t last_ltf_open_time = 0;    // dedup / gap - state-owned, see below
    bool    is_active = false;
};
```

`VolumeSurgeManager::Get()` returns the two **raw sums**, not their ratio: the threshold
test is `recent * 100 > prev * 130` in `__int128` (§3), so handing back a quotient would
mean dividing — losing exactness — only for the caller to multiply again. `natr_bp` is
an integer for the same reason, and because basis points are directly comparable across
symbols for the NATR rank.

### Why this decomposition

- **One class per condition** means each signal is independently unit-testable against a
  naive reference, with no engine, no config and no other signal present. That is what the
  DRILL mode wants, and it is why `SymbolState` is a plain aggregate with no logic of its
  own.
- **Each manager stores only what its own logic reads.** `VolumeSurgeManager` needs 48
  turnover values, `VolatilityManager` needs one prior close, `TrendManager` needs one
  close plus EMA state. Giving all three a full 48-candle history would triple the
  per-symbol footprint to hold data two of them never read.
- **`prev_close` is not a free-floating field.** Wilder's true range needs the previous
  close, and `VolatilityManager` owns that value because it is the only consumer. The
  candle *history* for later analysis lives once, in `recent_ltf` / `recent_htf`, and
  **no signal depends on it** — so a change to the history depth can never alter a
  signal.
- **`std::deque` in the MVP, a ring later.** A deque allocates per block, which the
  eventual no-alloc goal forbids — but that goal is Phase 1, gated on a benchmark, and
  the MVP's job is to be correct and measurable first. When it is replaced, capacity
  becomes a power of two so the wrap is a mask.
- **EMA50 and Wilder ATR keep no history.** Both are recursive, so they are a few scalars
  of state. This is also why a gap cannot be repaired by inserting the missing bar later
  (§7).

### Thresholds live in CoreManager, not in the managers

`Get()` returns the computed *value*; it does not decide. `TryActivate` / `TryDeactivate`
read the three values and compare them against the config thresholds:

```cpp
void CoreManager::ApplyCandle(const Candle& c) {
    SymbolState& s = state_[c.symbol_id];

    // 1. identity: dedup / gap, ONCE, before any manager sees the bar
    switch (Classify(c.open_time_ms, s.last_ltf_open_time)) {
        case kDuplicate: return;
        case kGap:       RequestRebuild(s); return;
        case kNext:      break;
    }
    s.last_ltf_open_time = c.open_time_ms;

    // 2. history (inspection only)
    s.recent_ltf.Push(c);

    // 3. LTF signals
    s.volume_surge.Update(c);
    s.volatility.Update(c);
    s.trend.UpdateLtf(c);

    // 4. HTF, BEFORE evaluating - see the ordering note in section 5
    if (const auto htf = s.agg.Add(c); htf) {
        s.recent_htf.Push(*htf);
        s.trend.UpdateHtf(*htf);
        ApplyCandleComplete(s, TimeFrame::kHtf4h);
    }

    // 5. decide
    Evaluate(s);
}
```

**Why thresholds stay out:** the managers become pure functions of candle history, so
their tests need no config; the three numbers live in one place; and `Evaluate` reads as
the filter table from §1, which is what the debrief has to show.

**Alternative (rejected):** `IsSatisfied()` inside each manager. Fewer lines at the call
site, but each manager then needs the config, and the definition of ACTIVE is spread over
three files with no single place to read it.

### Readiness is per signal

Each manager reports its own `ready`: 48 LTF bars for the surge, ~150 HTF bars for EMA50,
15 LTF bars for ATR(14). `is_ready` is the AND of the three.

**Why not one `is_valid` flag on the symbol:** a single flag has to encode the maximum of
three different warm-up requirements, and it silently becomes wrong the moment a period or
a window size changes in config. Each manager already knows how much history it has
consumed.

### The active set

```cpp
std::unordered_set<Symbol> active_instruments_;   // CoreManager-owned
```

Keyed by symbol string, on purpose: it is touched only on a *transition* (at most once per
symbol per hour) and it is read by the printer and the notifier, both of which want the
name. String hashing never reaches the message path, where dispatch is
`state_[candle.symbol_id]` — an array index.

**Rejected:** the two-map shape from `shema.md`
(`unordered_map<Symbol, CandleManager> tracked_` + `active_`). Holding `CandleManager`
*by value* in both means two copies of every signal, and "which copy is current" becomes a
question the design has to answer. One owner (`std::vector<SymbolState>`, indexed by id)
plus a set of *names* has one answer.

### Dedup state is owned by SymbolState, not by a manager

The three managers never see a bar the identity check rejected. If each tracked its own
`last_open_time`, a bar could be accepted by one and rejected by another, leaving the
symbol permanently inconsistent with no way to detect it. One owner, checked once, before
step 3.

---

## 7. Warm-up, gaps and rebuild

**[not implemented]** — this is the core correctness argument of the project.

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
   for each id, reset that symbol's three managers and aggregator, then run the same
   `Warmup()` call as at startup.
3. Any live bar that closed during the blocking REST call is still in the socket buffer.
   It arrives afterwards with an `open_time` the refetch already covered, so the dedup
   rule drops it. Nothing is lost and nothing is applied twice.

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

**[not implemented]** except `tests/unit_tests/src/test_spsc_queue.cpp`, which is reusable
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
- Each signal manager in isolation: `Get()` before readiness, the readiness boundary
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
3. **Bybit WS limits (§8)** — max args per subscribe request for `linear`, and max topics
   per connection. Both are undocumented for `linear` and the MVP assumes one connection
   holds the whole universe with 10 topics per subscribe frame. **Check on the first
   run.** (The "is 1–60 s a guarantee?" question is gone: nothing depends on it now that
   the data-silence watchdog is replaced by the missed-pong check.)
4. **Transition output** — console table only, or also a JSONL event log? The prompt's
   definition of done requires the table and "logs transitions"; a structured log is cheap
   and makes the demo reproducible.
5. **`screener_prompt.md` is still written against Binance** — the filter, the roadmap and
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
| Symbol state | `std::vector<SymbolState>`, indexed by id |
| Candle history | `std::deque<Candle>` / `std::deque<Volume>` |
| Candle | natural layout, `int64_t` fields, no size assert |
| Indicators | plain classes, no CRTP, integer math per §3 |
| Warm-up | **one REST call per symbol, `interval=60` only**; HTF aggregated locally (§7) |
| Connections | **one** WebSocket for the whole universe, batched subscribe |
| Liveness | missed-pong check on the mandatory 20 s ping; no data-silence watchdog |
| Rebuild | deferred to the main loop, blocking REST, reuses the warm-up path |
| Discard | parse the frame, check `confirm` — **no raw-string prefilter yet** |
| REST pacing | self-paced against 600/5 s, simple sleep-based limiter |
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
| Non-blocking REST | a rebuild stall is a few hundred ms and loses nothing | a rebuild storm makes the stall add up |

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
