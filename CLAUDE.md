# CLAUDE.md

Project instructions for Claude Code. Read this before doing anything.

## Project

C++20 real-time crypto screener, built in 2 days as HFT interview preparation.

It watches 500–1000 **Bybit USDT perpetual futures** (`category=linear`), builds LTF (1h)
and HTF (4h) candle state per symbol, and marks each symbol `ACTIVE` or `INACTIVE` on
closed LTF candles using a three-condition filter (volume surge, trend vs EMA50 on HTF,
NATR volatility).

Stack: C++20, Boost.Beast/Asio, OpenSSL, simdjson, fmt, GoogleTest, CMake + vcpkg.

The project is also Anton's vehicle for practising specific interview exercises, so **how
we work matters as much as what we build**. Anton must be able to explain every important
mechanism and trade-off in a debrief.

---

## Working Mode — DRILL vs SCAFFOLD

Every component is tagged in `screener_prompt.md` §4. The tag decides who writes it.

### [DRILL] — Anton writes it, cold. Claude must NOT write the implementation.

For a DRILL component, Claude:

1. gives the spec + interface + acceptance tests — **only when asked**;
2. states the time budget;
3. reviews Anton's code like a strict HFT interviewer: correctness, memory ordering, UB,
   layout, performance, naming;
4. asks 2–3 follow-up questions an interviewer would ask;
5. shows a reference implementation **only** after Anton's second attempt, or when he
   explicitly says "show solution" — then diffs it against his.

Writing a DRILL component unasked destroys the exercise. If a DRILL component is
blocking something else, say so and wait — do not fill it in.

DRILL components, split by phase (`DESIGN.md` §13):

- **Phase 0 (the domain):** `Candle`, `CandleTracker`/`SymbolTracker`, the integer indicators, the
  `CoreManager` loop + filter + transitions, and the `ControlManager` (partition,
  restart, rebuild).
- **Phase 1 (the optimisations, each with a benchmark either side):** the benchmark
  harness, the raw-frame prefilter, `FixedRing`, the 64-byte `Candle`, `FixedHashMap`,
  SPSC padding, CRTP indicators, the malloc-counting test, and `SpscQueue` rewritten cold.

### [SCAFFOLD] — Claude writes it fully, compile-ready, minimal and clean.

CMake/vcpkg/presets, config parsing, logging, REST client + rate limiter, WebSocket
plumbing including the ping timer, record/replay.

For `MdProvider` specifically: Claude writes the connection/subscription plumbing; Anton
writes the `x`-filter and the parse-to-`Candle` part, and Claude reviews it.

### MVP first, then measured optimisation

The project has two phases (`DESIGN.md` §13) and the boundary is deliberate.

**Phase 0 is the simplest correct screener that runs end to end.** Ordinary containers,
natural struct layout, no custom data structures, no layout tricks. It must be correct and
*measurable*, not fast. Do not pre-optimise Phase 0 code, and do not reach for a
fixed-capacity structure, a cache-line assert or a raw-string scan "while we are here".
If a simpler form would work for the MVP, write the simpler form.

**Phase 1 optimises it one change at a time, and every change is gated by a benchmark:**

1. State the suspected bottleneck.
2. Benchmark the baseline and record the number.
3. Make the smallest useful change.
4. Benchmark again and record the delta.
5. Verify the Phase 0 correctness tests still pass, and check tail latency.
6. Write the result down — **including when it is "no measurable difference"**.

A Phase 1 item that shows no gain is reported as such and may be reverted. That is a
result, not a failure: this system handles ~1000 updates per hour, so finding out *which*
optimisations are pointless here is the actual interview answer. Never present an
unmeasured change as an improvement.

### Explain before changing

Before any substantive implementation or design change:

- state what you want to change;
- explain why;
- mention the main alternative if there is a meaningful trade-off;
- explain the correctness/performance implications;
- mention what Anton should understand for the interview.

Then **wait for explicit approval**. Do not edit or create files without approval.

Read-only work needs no approval: inspecting and searching files, analysing code,
inspecting build configuration, running tests and benchmarks, reading compiler errors,
investigating performance.

After an approved change, stop and report before starting another independent change.
Prefer small, understandable changes.

### Session start

Ask which roadmap step (`screener_prompt.md` §6) Anton is on, and continue from there.

---

## Source of Truth

- `screener_prompt.md` — the assignment: filter definition, component modes, the 2-day
  roadmap, the cut list, and the interview questions to drill on. **Its API facts are
  Binance's and are wrong for this project** — `DESIGN.md` §8 is the only venue reference.
- `DESIGN.md` — current architecture, invariants, implementation status, rejected
  alternatives, open questions.
- `README.md` — to be written by Anton at step 10: architecture, threading model, the
  restart correctness argument, measured numbers, limitations, v2 plan.
- `shema.md` — Anton's original architecture sketch. **Historical.** Where it disagrees
  with `DESIGN.md`, `DESIGN.md` wins, and §6/§7 of `DESIGN.md` record why.

Read `DESIGN.md` before making architecture-dependent changes.

If code and documentation disagree, stop and report it. Never silently change an
established design decision. When a design changes, update `DESIGN.md` after approval.

Do not duplicate detailed project knowledge in `CLAUDE.md`.

---

## Architecture Invariants

Preserve these unless Anton explicitly approves a redesign. Full reasoning in `DESIGN.md`.

### Ownership and threading

- **The MVP is ONE thread and one `io_context`. No queues, no atomics, no joins.** The
  provider hands each parsed candle straight to `CoreManager::ApplyCandle` through a
  direct callback. Do not add a thread, a queue or an atomic to Phase 0.
- **That callback is safe only because there is one thread.** The moment parsing moves to
  its own thread, a direct call into `CoreManager` is a data race on every indicator.
  That migration (callback → SPSC push) is Phase 1 item 10 and needs a measurement first;
  `md_core/spsc_queue.h` is kept in the tree for it and is currently unused.
- `CoreManager` (`md_core/`) owns symbol state and `TryActivate`/`TryDeactivate`;
  `ControlManager` (`control_manager/`) owns startup, REST and rebuilds. They share the
  thread in the MVP but keep separate roles — do not fold the lifecycle work into the
  state owner.
- **A rebuild is never run inline.** A gap records the symbol id and returns; the main loop
  drains the pending list. Calling the warm-up path from inside `ApplyCandle` re-enters it
  per history bar while the outer call is mid-flight.
- **REST is async on the `io_context` thread**, except `instruments-info`, which blocks
  before `ioc.run()` is entered. Warm-up overlaps the live stream deliberately: the
  subscribe comes first and frames land in `ControlManager::startup_buffer_` until the
  last warm-up response replays them. Blocking the one thread would starve the socket the
  buffer exists to protect. `RateLimiter` has `Acquire()` and `AsyncAcquire()` over one
  shared window — do not give them separate windows.
- Prefer ownership and message passing over shared mutable state.

### Market data

- One venue, one market: **Bybit USDT perpetuals**, `category=linear`,
  `contractType=LinearPerpetual`. Never connect to `/v5/public/spot`.
- Exchange protocol details (`confirm`, envelope `ts`, `start`, `kline.60.{SYMBOL}` topic
  naming, uppercase symbols, the array-of-arrays REST kline shape) stay inside
  `md_provider`. `md_core` consumes normalised `Candle` values only.
- **`open_time` is the only identity of a bar** (Bybit `start`): dedup, gap detection and
  HTF alignment all key on it. `event_time` (the envelope `ts`) describes the message,
  never the bar — it is a latency input only. `data[].timestamp` is the last matched
  order's time and is used for nothing.
- We subscribe to the **LTF (1h) kline only**; the HTF (4h) is aggregated locally. Each
  completed candle routes through `ApplyCandleComplete(TimeFrame)`.
- Act on `confirm == true` only. The MVP parses and checks it; the raw-frame scan is a
  Phase 1 optimisation. When it exists it is a PREFILTER, not the decision — `data` is an
  array, so the parser still checks `confirm` per element.
- **Bybit returns REST klines newest-first.** Warm-up and rebuild must iterate
  `result.list` in reverse. Applying them as received feeds the recursive indicators
  backwards and produces values that look plausible and are wrong.
- The pong IS the liveness signal (there is no data-silence watchdog to confuse it with).
  N unanswered pings means reconnect; a pong resets the counter.
- **A gap invalidates the symbol and forces a full rebuild from REST.** The indicators are
  recursive, so a missing bar cannot be patched in afterwards. Dedup by `open_time` makes
  a backfill idempotent; it does not fill a hole.
- **Warm-up fetches `interval=60` only.** The HTF history is built by the same aggregator
  that builds it live, so warm-up and live HTF bars can never disagree. One REST call per
  symbol (~603 bars, sized by EMA50's warm-up), and the leading partial 4h group must be
  discarded or the first EMA value is silently biased.
- **No data-silence watchdog.** One connection carries every symbol, and the threshold
  would be a guess. Liveness is the missed-pong check on the mandatory 20 s ping, plus
  Beast's error callback. Do not reintroduce a market-activity timer.
- A symbol with insufficient history is `not ready` and can never be ACTIVE.
- Symbol strings never appear on the message path. A `Candle` carries a `uint32_t` id;
  dispatch is an array index.
- **State is decomposed by TIMEFRAME, not by filter condition.** `SymbolTracker` owns one
  `CandleTracker` per timeframe; each `CandleTracker` owns the indicators its own
  timeframe drives (LTF: turnover window + ATR; HTF: EMA50). Every indicator belongs to
  exactly one timeframe — the per-condition split was rejected because the trend
  condition spans both, so a `TrendManager` needed `UpdateLtf()` *and* `UpdateHtf()`.
  `DESIGN.md` §6 records the full argument.
- **`Get()` returns a VALUE and never a verdict** — thresholds live in `CoreManager`, so
  changing one never means touching an indicator. The three conditions are reads there:
  surge from `ltf.Turnover()`, trend from `ltf.LastClose()` vs `htf.GetEma()`, volatility
  from `ltf.GetNatrBp()`.
- Indicators are **named `std::optional` members**, never a
  `vector<unique_ptr<IndicatorBase>>`. A single `virtual double GetValue()` cannot carry a
  typed `Price`, an `int32_t` basis-point NATR and a *pair* of sums, and Wilder's ATR
  cannot implement `virtual void Update(double)` at all — True Range needs high, low and
  the previous close.
- `has_value()` ("this timeframe uses the indicator") and `Ready()` ("it has enough
  history") are different questions. Readiness is a bar counter and is what makes a symbol
  `not ready`. Dedup/gap state belongs to `SymbolTracker`, not to an indicator.

### Numeric representation

- **Scaled integers everywhere. No floating point in this system at all**, including
  inside the indicators. `Price` and `Volume` are both `int64_t`.
- Two scales, each justified by its own magnitude range: `kPriceScale = 1e10` (the perp
  universe spans ~10^10, from BTC to the 1000-multiplier contracts) and
  `kVolumeScale = 1e6` (turnover is always USDT and is *summed*, so it needs overflow
  headroom, not magnitude headroom). Do not unify them.
- Parse decimal strings straight to scaled integers with `ParseScaledDecimal` — never via
  a double.
- Thresholds are integer comparisons: surge is `recent * 100 > prev * 130`, NATR is
  carried in **basis points** and compared against `100`. Never divide to make a ratio
  the caller will multiply again.
- `__int128` is required for the NATR expression and the surge comparison — both overflow
  `int64_t` at BTC scale. The ATR and EMA recursions do not.
- The recursive indicators use **rounding division, not truncation**, and the resulting
  error budget (~13 units, ~0.02 % for the cheapest symbol) is why indicator tests use a
  stated tolerance against a floating-point reference rather than `1e-9`.
- See `DESIGN.md` §3 for the full argument, including why a per-symbol `tickSize` scale
  was rejected for the MVP.

### Allocation

- Zero heap allocation on the message path is a **Phase 1 goal**, not an MVP invariant.
  The MVP uses ordinary containers deliberately, so Phase 1 has a baseline to measure
  against. When it lands it is verified by a malloc-counting test, never by inspection.

---

## Performance

Performance decisions must be evidence-based. The benchmark harness is Phase 1 item 1
because nothing after it can be justified without a baseline.

1. Identify the suspected bottleneck.
2. Measure it.
3. Make the smallest useful change.
4. Benchmark again.
5. Verify correctness and tail latency.
6. Record the before/after pair, whichever way it came out.

Clearly distinguish **measured**, **estimated**, and **hypothesized**. Never claim an
optimisation is faster without measurement, and never invent a number.

A surprising benchmark result is evidence to investigate, not something to rationalise
away.

Avoid complexity added for theoretical performance: custom allocators, lock-free
structures, SIMD, extra threads, extra copies, serialisation,
synchronisation. If an optimisation is suggested, say what would be measured to justify
it.

Remember the honest framing: a screener decides on an hourly close, so the latency work
is justified by the exercise list, not by the business requirement. Say that rather than
overselling it.

---

## C++ Style

C++20, `-Wall -Wextra -Werror`. Follow the existing project style.

Prefer: RAII; explicit ownership; `std::unique_ptr` for exclusive ownership;
`std::shared_ptr` only when shared lifetime is genuinely required; `std::string_view` /
`std::span` for non-owning views; `enum class`; `constexpr`; `noexcept` where justified;
standard containers unless measurement or the no-alloc rule requires otherwise.

Avoid unnecessary abstraction and generalisation. CRTP where the exercise list calls for
it; no `std::function` on the per-bar path.

Use the project's `Logger`. Do not log from tight hot paths.

Use `clang-format` for C++ changes. Do not add dependencies beyond those in `vcpkg.json`.

---

## Testing

Tests are written alongside implementation, not at the end.

For meaningful changes: build the affected targets, run the relevant tests, run
benchmarks for performance-sensitive changes, and inspect the final diff.

Every DRILL component has unit tests. Concurrency components also have a TSan stress test.

Builds: debug with ASan+UBSan, and a separate TSan build for the concurrency tests
(`CMakePresets.json`).

For core/candle changes, verify: dedup of duplicate `open_time`; the three arrival cases
(older, exact next, gap); gap → invalidate → rebuild; warm-up buffering across an hour
boundary; HTF aggregation boundaries and the apply-before-evaluate ordering; REST klines
applied in reverse;
ACTIVE/INACTIVE transition edges; `not ready` never going ACTIVE; indicators against a
naive reference within the stated tolerance from `DESIGN.md` §3 (not 1e-9 — the integer
indicators are deliberately not bit-identical to a double reference); both price extremes
(BTC scale and ~0.000006); deterministic replay.

---

## Provider Rules

`md_provider` owns: the WebSocket connection to `wss://stream.bybit.com/v5/public/linear`,
batched and paced `{"op":"subscribe"}` frames, the **20 s client ping timer**, the cheap
`confirm` prefilter, simdjson parsing, normalisation into `Candle`, reconnect with
exponential backoff and jitter, the missed-pong liveness check, REST
(`instruments-info` with cursor pagination, `kline`) and the rate limiter, and
record/replay.

The rate limiter self-paces against **600 requests / 5 s per IP**. It does NOT trust the
`X-Bapi-Limit-*` headers: those report the per-UID endpoint quota, not the IP limit our
unauthenticated market calls are bound by. On `403 access too frequent` / `retCode 10006`,
back off at least 10 minutes — that is the stated unban delay, and retrying sooner
extends the ban.

`instruments-info` defaults to 500 entries and there are more than 500 linear symbols, so
`cursor` pagination is mandatory — a single default request silently truncates the
universe.

REST is blocking and must never run on a provider's `io_context` thread.

Do not leak Bybit protocol semantics into `md_core`.

---

## Documentation

Keep documentation aligned with implementation. `README.md` for high-level decisions and
measured numbers; `DESIGN.md` for current technical design, experiments, failures and
lessons. Do not turn `CLAUDE.md` into a project encyclopedia.

---

## Communication

Answer questions directly before proposing implementation.

Keep answers short and concrete. No long explanations unless Anton asks "why".

For a non-trivial change, use: **Change → Why → Alternative → Trade-offs →
Test/Benchmark**.

Use simple English and concrete examples.

When a meaningful implementation is completed, include:

### You should now be able to explain

2–4 likely senior-level interview questions about the mechanism just changed.
`screener_prompt.md` §8 lists the questions to keep returning to.

---

## Never

- Never write a [DRILL] component's implementation unless Anton asks for the solution.
- Never write code before approval.
- Never silently change architecture.
- Never invent benchmark numbers.
- Never mix spot and futures data.
- Never let a provider thread write a `SymbolTracker` (and in the MVP, never create one).
- Never patch a gap by inserting a late bar into recursive indicator state.
- Never put a symbol string or an allocation on the message path.
- Never optimise without measurement, and never optimise Phase 0.
- Never add a thread, a queue or an atomic to the MVP.
- Never run a rebuild inline from inside `ApplyCandle`.
- Never put floating point into a price, a volume, or an indicator.
- Never add unnecessary complexity.
- Never leave code/docs inconsistent.
- Never claim something was tested, measured, implemented, or fixed unless it actually
  was.
