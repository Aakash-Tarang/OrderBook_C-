#!/usr/bin/env python3
"""
generate_market_data.py

Generates a realistic simulated market data feed as a CSV file.

Design choices:
  - Orders cluster around a mid-price that drifts randomly (Brownian motion),
    mimicking real quote clustering near the NBBO.
  - Cancellations target recently-added live orders, reflecting real cancel
    rates (often 90%+ in HFT environments).
  - BID prices are constrained below mid; ASK prices above mid, preserving
    a realistic spread.
  - A configurable cancel_ratio controls how aggressive the cancellation
    activity is relative to adds.
"""

import csv
import random
import math

# ── Configuration ─────────────────────────────────────────────────────────────
SEED            = 42
N_ORDERS        = 500_000      # total rows in the CSV
CANCEL_RATIO    = 0.45         # 45% of operations are cancellations
MID_PRICE_START = 10_000       # starting mid-price (in integer ticks)
SPREAD_TICKS    = 4            # half-spread: bids ≤ mid-2, asks ≥ mid+2
JITTER_TICKS    = 10           # max random offset from mid for each order
DRIFT_STD       = 0.5          # std-dev of mid-price random walk per step
OUTPUT_FILE     = "market_data.csv"
# ──────────────────────────────────────────────────────────────────────────────


def clamp(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, value))


def generate(
    n_orders: int,
    cancel_ratio: float,
    mid_start: int,
    spread: int,
    jitter: int,
    drift_std: float,
    seed: int,
    output_path: str,
) -> None:
    rng = random.Random(seed)

    mid = mid_start
    next_order_id: int = 1

    # live_orders tracks {order_id -> (side, price, qty)} so we can emit
    # valid cancel rows that reference an order currently in the book.
    live_orders: dict[int, tuple[str, int, int]] = {}

    # We keep an ordered list of IDs for O(1) random sampling without
    # having to iterate the whole dict.
    live_ids: list[int] = []

    rows: list[tuple] = []

    for _ in range(n_orders):
        # ── Drift mid-price (bounded random walk) ─────────────────────────
        # Using a Gaussian step keeps the series smooth and avoids wild jumps.
        mid += int(rng.gauss(0, drift_std))
        mid = clamp(mid, mid_start - 500, mid_start + 500)  # soft boundary

        # ── Decide: Add or Cancel ──────────────────────────────────────────
        # We only emit a cancel if there is at least one live order to cancel;
        # otherwise we fall back to an add, preventing referencing phantom IDs.
        do_cancel = live_ids and rng.random() < cancel_ratio

        if do_cancel:
            # Pick a random live order to cancel (uniform sampling).
            idx        = rng.randrange(len(live_ids))
            cancel_id  = live_ids[idx]

            # Swap-and-pop for O(1) removal from the live list.
            live_ids[idx] = live_ids[-1]
            live_ids.pop()
            info = live_orders.pop(cancel_id)

            side  = info[0]
            price = info[1]
            qty   = info[2]

            # Cancel rows: same side/price/qty as the original add so the
            # consumer can forward them to OrderBook::cancel_order by ID.
            rows.append(("C", cancel_id, side, price, qty))

        else:
            # ── Construct a new Add order ──────────────────────────────────
            # Side is chosen uniformly; price is clustered around mid with a
            # half-spread gap to keep bids < asks in aggregate.
            side = "BID" if rng.random() < 0.5 else "ASK"

            if side == "BID":
                # BIDs sit below mid by at least half the spread.
                base  = mid - spread
                price = clamp(base + rng.randint(-jitter, 0), 1, mid_start + 1000)
            else:
                # ASKs sit above mid by at least half the spread.
                base  = mid + spread
                price = clamp(base + rng.randint(0, jitter), 1, mid_start + 1000)

            qty      = rng.randint(1, 200)
            order_id = next_order_id
            next_order_id += 1

            live_orders[order_id] = (side, price, qty)
            live_ids.append(order_id)

            rows.append(("A", order_id, side, price, qty))

    # ── Write CSV ──────────────────────────────────────────────────────────────
    with open(output_path, "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["action", "order_id", "side", "price", "qty"])
        writer.writerows(rows)

    n_adds    = sum(1 for r in rows if r[0] == "A")
    n_cancels = sum(1 for r in rows if r[0] == "C")
    print(f"Written {len(rows):,} rows to '{output_path}'")
    print(f"  Adds    : {n_adds:,}")
    print(f"  Cancels : {n_cancels:,}")
    print(f"  Final mid-price: {mid}")


if __name__ == "__main__":
    generate(
        n_orders    = N_ORDERS,
        cancel_ratio= CANCEL_RATIO,
        mid_start   = MID_PRICE_START,
        spread      = SPREAD_TICKS,
        jitter      = JITTER_TICKS,
        drift_std   = DRIFT_STD,
        seed        = SEED,
        output_path = OUTPUT_FILE,
    )