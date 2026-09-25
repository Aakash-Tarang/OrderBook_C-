# Archive – historical iterations

These folders are the unmodified (except where noted) implementations from
the learning journey described in `docs/report/report.tex`.  They are kept
for provenance and comparison – **do not build new work on top of them**.

The live engine lives in `include/ob/`, `src/`, `bench/`, `tests/` and is
described in the top-level `README.md`.

> **Portability note.** These historical sources were written against an
> Apple M1 Mac and contain x86-only inline assembly (`pause`) and x86
> intrinsics assumptions in the benchmark timing paths (`rdtsc`-era
> thinking with `high_resolution_clock`).  They still *compile* with GCC on
> x86-64 Linux; on Apple Silicon the timing layers should be considered
> unreliable (that is precisely what the sixth iteration fixes with
> `include/ob/timing.h`).  The archive is documented history, not a
> reference implementation.

| Folder | Phase | Core data structure | Report section |
|--------|-------|--------------------|----------------|
| `00_direct_measurement` | phase 0 | `priority_queue` + first latency harness | §2 (background) |
| `01_cplus_implementaiton` | phase 1 | `priority_queue` book + TCP client/server loopback benchmark | §2 |
| `02_cplus_second_attemp` | phase 2 | price-indexed level vector, lazy cancellation | §3 |
| `03_cplus_third` | phase 3 | `std::map` levels + `std::list` FIFO + iterator-stable registry | §4 |
| `04_cplus_fourth` | phase 4 | flat level array + 1-level bitset + pool + intrusive lists | §5 |
| `05_cplus_fifth` | phase 5 | 2-level bitset + SPSC ring pipeline (CSV-fed) | §6 |

Historical benchmark CSVs/PNGs are committed (small); the multi-megabyte
raw latency dumps and the 11 MB `market_data.csv` were removed – regenerate
with the phase scripts if needed.
