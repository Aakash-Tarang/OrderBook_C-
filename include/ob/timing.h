#pragma once
/*
 * timing.h – Portable low-overhead timing for x86-64 and ARM64 (Apple Silicon).
 *
 * Why this exists
 * ───────────────
 * The previous benchmarks called std::chrono::high_resolution_clock::now()
 * twice per operation.  That call is a vDSO clock_gettime (~20–30 ns on
 * typical systems) – i.e. the measurement overhead was of the same order of
 * magnitude as the ~40 ns operations being measured.
 *
 * This header provides a hardware cycle counter with a *much* cheaper read:
 *
 *   x86-64 : RDTSCP (partially serialising; waits for prior instructions)
 *   ARM64  : CNTVCT_EL0 (generic virtual timer, read via MRS)
 *   other  : falls back to std::chrono::steady_clock (ns domain)
 *
 * The raw counter runs in *ticks*, whose frequency is NOT the CPU frequency
 * (RDTSC runs at a fixed base clock; CNTVCT at CNTFRQ_EL0 – 1 GHz on Apple
 * M-series, but as low as 24 MHz on some other ARM parts).  Ticks are
 * calibrated to nanoseconds once, lazily, by comparing counter progress
 * against steady_clock over a few ~8 ms windows.
 *
 * Honest measurement (see report §7):
 *   - counter_bracket_overhead_ns() reports the cost of one empty
 *     read-bracket pair, so every published number can be quoted together
 *     with its measurement overhead.
 *   - When the counter is too coarse for per-op timing (tick period > ~5 ns,
 *     e.g. 24 MHz timers), use batch timing instead: time N ops with
 *     steady_now_ns() and divide by N.
 *
 * All functions are single-thread-safe.  For cross-thread ingest stamps
 * (tick-to-trade) producer and consumer of the same process share one
 * counter domain, which is the only guarantee we need.
 */

#include <chrono>
#include <cstdint>
#include <thread>

namespace ob {

#if defined(__x86_64__) || defined(_M_X64)
#define OB_HAS_HW_COUNTER 1
#elif defined(__aarch64__)
#define OB_HAS_HW_COUNTER 1
#else
#define OB_HAS_HW_COUNTER 0
#endif

// ── Raw counter read ─────────────────────────────────────────────────────────
//
// Returns hardware ticks (nanoseconds on the steady_clock fallback).
// On x86-64 RDTSCP is used because it does not begin until all prior
// instructions have completed – the bracket stays honest about what it wraps.
// On ARM64 the generic timer is a plain MRS read (single-digit ns).
//
#if defined(__x86_64__) || defined(_M_X64)
inline uint64_t read_counter() noexcept {
    unsigned aux;
    return __builtin_ia32_rdtscp(&aux);
}
#elif defined(__aarch64__)
inline uint64_t read_counter() noexcept {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
inline uint64_t read_counter() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif

// True when read_counter() is a hardware counter (not the steady_clock
// fallback).  When false, ticks == ns and conversion factors are 1.0.
inline bool has_hw_counter() noexcept { return OB_HAS_HW_COUNTER != 0; }

// ── Spin-wait hint (replaces the x86-only `asm volatile("pause")`) ───────────
//
// x86   : PAUSE – avoids memory-order speculation penalties in spin loops.
// ARM64 : YIELD – hints the core can make progress elsewhere.
// other : no-op.
//
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    /* no-op */
#endif
}

// ── Wall-clock nanoseconds (monotonic) ───────────────────────────────────────
// Used for batch timing and calibration, not for per-op brackets.
inline uint64_t steady_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace detail {

struct Calibration {
    double ns_per_tick        = 1.0; // ticks → ns
    double bracket_overhead_ns = 0.0; // cost of one empty read pair, in ns
};

// Computed once on first use.
//
// ns_per_tick : median of several (wall_ns / counter_ticks) samples taken
//               over ~8 ms sleeps, so the ratio is robust to scheduler noise.
// overhead    : minimum empty-bracket width over 2000 tries, converted to ns.
//               min() because any single sample can only be inflated by noise.
inline Calibration calibrate() {
    Calibration c{};

#if OB_HAS_HW_COUNTER
    // ── Empty-bracket overhead (in raw ticks for now) ────────────────────────
    uint64_t min_bracket_ticks = ~0ULL;
    {
        constexpr int kIters = 2000;
        for (int i = 0; i < kIters; ++i) {
            uint64_t t0 = read_counter();
            uint64_t t1 = read_counter();
            uint64_t d  = t1 - t0;
            if (d < min_bracket_ticks) min_bracket_ticks = d;
        }
    }

    // ── ns per tick, median of 7 samples ─────────────────────────────────────
    {
        constexpr int kSamples = 7;
        double       ratios[kSamples];
        int          used = 0;
        for (int i = 0; i < kSamples; ++i) {
            auto     w0 = std::chrono::steady_clock::now();
            uint64_t c0 = read_counter();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            uint64_t c1 = read_counter();
            auto     w1 = std::chrono::steady_clock::now();

            const double wall_ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0)
                    .count());
            const double ticks = static_cast<double>(c1 - c0);
            if (ticks > 0.0 && wall_ns > 0.0)
                ratios[used++] = wall_ns / ticks;
        }

        double ns_per_tick = 1.0;
        if (used > 0) {
            // insertion sort (n ≤ 7) then take the median
            for (int i = 1; i < used; ++i) {
                double v = ratios[i];
                int    j = i - 1;
                while (j >= 0 && ratios[j] > v) {
                    ratios[j + 1] = ratios[j];
                    --j;
                }
                ratios[j + 1] = v;
            }
            ns_per_tick = ratios[used / 2];
        }

        c.ns_per_tick         = ns_per_tick;
        c.bracket_overhead_ns = static_cast<double>(min_bracket_ticks) *
                                ns_per_tick;
    }
#else
    c.ns_per_tick          = 1.0;
    c.bracket_overhead_ns  = 0.0; // fallback: overhead not separable
#endif
    return c;
}

inline const Calibration& calibration() {
    static const Calibration cal = calibrate();
    return cal;
}

} // namespace detail

// Ticks → nanoseconds (no-op when ticks are already ns).
inline double counter_to_ns(uint64_t ticks) noexcept {
    return static_cast<double>(ticks) * detail::calibration().ns_per_tick;
}

// Cost of one empty read_counter() pair, in ns.  Quote this next to any
// per-op bracket measurement.
inline double counter_bracket_overhead_ns() noexcept {
    return detail::calibration().bracket_overhead_ns;
}

// Counter resolution in ns (tick period).  Per-op bracketing is only
// meaningful when this is well below the thing being measured.
inline double counter_resolution_ns() noexcept {
    return detail::calibration().ns_per_tick;
}

} // namespace ob
