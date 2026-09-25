#pragma once
/*
 * ring_buffer.h – Cache-line-aware, lock-free SPSC ring buffer.
 *
 * Design principles
 * ─────────────────
 * 1. SINGLE PRODUCER / SINGLE CONSUMER only.
 *    Using relaxed/release/acquire memory orders is safe ONLY because
 *    at most one thread writes write_pos_ and at most one thread writes
 *    read_pos_.  Adding a second producer/consumer breaks correctness.
 *
 * 2. Power-of-two capacity → index wrap with bitwise AND (no modulo).
 *    Modulo with a runtime divisor compiles to a division instruction on
 *    x86; bitwise AND is a single cycle.  We static_assert the constraint.
 *
 * 3. Cache-line isolation (alignas(64)).
 *    write_pos_ is written by the producer and read by the consumer.
 *    read_pos_ is written by the consumer and read by the producer.
 *    Placing them on separate 64-byte cache lines eliminates false sharing,
 *    which would otherwise force a cache-coherence round-trip on every op.
 *
 * 4. Memory ordering
 *    - load(relaxed) on the index we OWN  → no fence needed; only one
 *      thread mutates it.
 *    - load(acquire) on the index we OBSERVE → synchronises with the
 *      store(release) on the other thread, guaranteeing the slot data
 *      is visible before we read/overwrite it.
 *    - store(release) after writing/reading the slot → makes the data
 *      visible to the other thread before the index update becomes visible.
 *
 * 5. Slot data is copied (trivially copyable T recommended).
 *    For HFT payloads this is intentional – the producer might reuse its
 *    local struct immediately after push() returns.
 *
 * Template parameters
 * ───────────────────
 *  T         – element type.  Must be copyable/movable.
 *  Capacity  – ring size in elements.  MUST be a power of two.
 *
 * Portability: this header contains no architecture-specific code.  Spin
 * loops live in the callers, which should use ob::spin_pause() (PAUSE on
 * x86-64, YIELD on ARM64 / Apple Silicon) between failed push/pop attempts.
 */

#include <atomic>
#include <cstddef>
#include <cassert>
#include <type_traits>

namespace ob {

// Compile-time check: is N a power of two?
template <std::size_t N>
struct is_power_of_two
    : std::bool_constant<(N > 0) && ((N & (N - 1)) == 0)> {};


template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert(is_power_of_two<Capacity>::value,
                  "SPSCRingBuffer: Capacity must be a power of two "
                  "(e.g. 1024, 2048, 65536) to enable fast bitwise wrap.");

    // Mask used instead of modulo: index & MASK == index % Capacity
    // because Capacity is a power of two.
    static constexpr std::size_t MASK = Capacity - 1;

public:
    SPSCRingBuffer()  = default;
    ~SPSCRingBuffer() = default;

    // Non-copyable, non-movable – the buffer lives at a fixed address so
    // both threads can hold raw pointers/references to it safely.
    SPSCRingBuffer(const SPSCRingBuffer&)            = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&)                 = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&)      = delete;

    // ── push ──────────────────────────────────────────────────────────────
    // Called exclusively by the PRODUCER thread.
    // Returns true  → item enqueued.
    // Returns false → buffer full; caller should spin/yield and retry.
    [[nodiscard]] bool push(const T& item) noexcept {
        // relaxed: we are the only writer of write_pos_
        const std::size_t write = write_pos_.load(std::memory_order_relaxed);
        const std::size_t next  = (write + 1) & MASK;

        // acquire: synchronise with the consumer's release-store of read_pos_
        // so we correctly see whether the slot we want to write is free.
        if (next == read_pos_.load(std::memory_order_acquire)) {
            return false; // full – one slot is always sacrificed as a sentinel
        }

        buffer_[write] = item; // write payload BEFORE advancing index

        // release: make the payload write visible to the consumer before
        // the index update.
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // ── pop ───────────────────────────────────────────────────────────────
    // Called exclusively by the CONSUMER thread.
    // Returns true  → item dequeued into `out`.
    // Returns false → buffer empty; caller should spin and retry.
    [[nodiscard]] bool pop(T& out) noexcept {
        // relaxed: we are the only writer of read_pos_
        const std::size_t read = read_pos_.load(std::memory_order_relaxed);

        // acquire: synchronise with the producer's release-store of write_pos_
        if (read == write_pos_.load(std::memory_order_acquire)) {
            return false; // empty
        }

        out = buffer_[read]; // read payload BEFORE advancing index

        // release: let the producer know this slot is now free
        read_pos_.store((read + 1) & MASK, std::memory_order_release);
        return true;
    }

    // ── size_approx ───────────────────────────────────────────────────────
    // Returns an *approximate* element count.  Exact only when called from
    // a single thread; treat as a diagnostic hint only.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t w = write_pos_.load(std::memory_order_relaxed);
        const std::size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w - r) & MASK; // works because Capacity is power-of-two
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    // ── Data slots ────────────────────────────────────────────────────────
    // Aligned to avoid splitting a slot across two cache lines where possible.
    alignas(64) T buffer_[Capacity];

    // ── Producer-owned index (written by producer, read by consumer) ──────
    // Padding to a full cache line prevents false sharing with read_pos_.
    alignas(64) std::atomic<std::size_t> write_pos_{0};

    // ── Consumer-owned index (written by consumer, read by producer) ──────
    alignas(64) std::atomic<std::size_t> read_pos_{0};
};

} // namespace ob
