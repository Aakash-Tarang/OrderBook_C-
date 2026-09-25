# OrderBook C++ — a limit order book, from naive to hardware-sympathetic

This repository tracks the evolution of a **price-time-priority limit order
book** written in C++20, from a `std::priority_queue` first attempt (~µs
latencies) to a pooled, branch-free, fuzz-tested engine operating in the
tens of nanoseconds — plus the measurement rigour (portable timing, raw
percentiles, differential fuzzing, sanitizers) needed to trust any of those
numbers.

The full narrative — including every design mistake — is in
[`docs/report/report.tex`](docs/report/report.tex) (compiled:
`latexmk -pdf` inside `docs/report/`).

---

## Repository layout

```
OrderBook_C-/
├── include/ob/            Live engine – headers
│   ├── orderbook.h        Price-time book: hierarchical bitset + node pool
│   ├── flat_id_map.h      Open-addressed uint64→int32 map (backshift deletion)
│   ├── timing.h           RDTSCP / CNTVCT_EL0 portable counter (Apple Silicon safe)
│   ├── ring_buffer.h      Lock-free SPSC ring, cache-line isolated
│   ├── replay_format.h    32-byte binary market-data replay records
│   └── reference_book.h   Deliberately naive std::map oracle (for fuzzing)
├── src/
│   ├── orderbook.cpp      Engine implementation
│   └── pipeline_main.cpp  matching_engine: producer → SPSC ring → engine
├── bench/benchmark.cpp    ob_bench: dense/sparse micro-benchmarks
├── tests/                 29-test suite + differential fuzzer (no deps)
├── scripts/
│   ├── generate_market_data.py   CSV + binary replay generator
│   └── plot_bench.py             report figures from results/*.csv
├── data/                  Generated feed files (small sample committed)
├── results/               Benchmark summaries + figures (raw dumps gitignored)
├── docs/report/           LaTeX report + figures/
├── archive/               Phases 0–5, preserved as history (see archive/README.md)
├── CMakeLists.txt         One command builds everything (Linux/macOS, x86-64/arm64)
└── .github/workflows/     CI: Ubuntu + macOS (Apple Silicon) build & test
```

## Building (Linux & macOS, x86-64 & Apple Silicon)

Requires CMake ≥ 3.16 and a C++20 compiler (GCC ≥ 10, Clang ≥ 13 / Apple
clang ≥ 13).  Nothing else — no third-party dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Targets:

| Target            | Purpose |
|-------------------|---------|
| `ob_tests`        | test binary (unit + differential fuzz) |
| `ob_bench`        | dense/sparse micro-benchmark (`--with-reference` adds the naive oracle) |
| `matching_engine` | SPSC pipeline over a feed file (CSV or binary replay) |

Sanitizer build (tests under ASan + UBSan):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOB_ENABLE_SANITIZERS=ON
cmake --build build-asan -j && ./build-asan/ob_tests
```

## Running the tests

```bash
./build/ob_tests
```

The suite covers the hierarchical bitset (vs a naive oracle, randomised),
the flat hash map (vs `std::unordered_map`, randomised), book edge cases
(self-cross, pool exhaustion, replace semantics, market-order sweeps), and
**differential fuzzing**: the optimised engine and the naive reference book
are driven by identical random op streams and must produce byte-identical
trade prints and identical book state after *every* operation.

Current status: **29/29 pass** in Release, Debug, and Debug+ASan/UBSan.

## Running the benchmarks

```bash
./build/ob_bench --with-reference          # writes results/bench_summary.csv
./build/matching_engine --input data/market_data.bin   # SPSC pipeline
python3 scripts/generate_market_data.py    # (re)generate data/ feed files
python3 scripts/plot_bench.py              # regenerate docs/report/figures/
```

### Latest results

Measured on a 2-core Intel Xeon 2.6 GHz cloud VM (the repo author's Apple M1
numbers will differ — re-run locally; the *relative* fast-vs-reference gap
is the meaningful signal).  Raw, **unfiltered** percentiles, hardware-counter
bracket overhead ≈ 14 ns (printed by the tools on every run):

| Workload | Engine | P50 | P90 | P99  | P99.9 |
|----------|--------|----:|----:|-----:|------:|
| dense    | `ob::OrderBook` | 69 ns | 135 ns | 255 ns | 415 ns |
| dense    | naive reference | 103 ns | 247 ns | 428 ns | 1 196 ns |
| sparse   | `ob::OrderBook` | 62 ns | 121 ns | 204 ns | 367 ns |
| sparse   | naive reference | 110 ns | 252 ns | 445 ns | 673 ns |

Block-throughput (steady_clock over 20k-op blocks): dense 33 ns/op,
sparse 41 ns/op.  Run-to-run spread on this shared VM is ±10-15%; the
committed `results/bench_summary.csv` is the canonical run quoted in the
report.

Two results worth calling out:

* **The phase-4/5 sparse-tail regression is gone.**  The two-level bitset
  keeps sparse P99 (204 ns) *below* the dense P99 (255 ns) — the old
  single-level bitset measured 667 ns there (report §5.7).
* **Historical tables were not comparable.**  Phases 2–4 were measured with
  two `high_resolution_clock::now()` calls (≈ 20–30 ns of overhead *per
  call*) and a Tukey outlier filter that deleted most of the tail.  Numbers
  in this README come from the corrected methodology of report §7 — treat
  all cross-phase tables as directional only.

## The engine in one paragraph

Price levels live in two flat vectors (bids/asks) indexed by price tick;
each level is an intrusive doubly-linked FIFO of order nodes carved out of
one pre-allocated pool (no heap allocation after construction); the best
price is found with a two-level hierarchical bitset (CTZ/CLZ jumps, 4096
ticks per summary-word step); order-id lookup uses a flat open-addressed
map with backshift deletion (the `std::unordered_map` it replaced allocated
a node on every add); trades are appended to a caller-owned vector (the
engine never returns a fresh allocation); rejections are return codes, not
exceptions.  `replace_order` (cancel + re-insert, time priority lost) and
market orders are supported; a debug-only `verify_invariants()` checks
twelve structural properties and is fuzzed after every operation.

## Report & figures

* `docs/report/report.tex` — the full write-up: phases 1–5 preserved,
  §6 (SPSC pipeline) and §7 (corrections, testing, re-measurement) added.
* `docs/report/figures/` — all figures referenced by the report, generated
  by `scripts/plot_bench.py` from `results/*.csv`.

## Roadmap

See the end of the report (§8).  Short version: ITCH/OUCH feed handlers,
journaling + deterministic replay, IOC/FOK time-in-force, fixed-point
prices, multi-instrument sharding, then the deep HFT stack (NUMA, hugepages,
kernel bypass).

## Historical phases

Phases 0–5 are preserved under [`archive/`](archive/README.md) — unmodified
except for the removal of regenerable artifacts.  Their latency numbers are
quoted (and where necessary corrected) in the report.
