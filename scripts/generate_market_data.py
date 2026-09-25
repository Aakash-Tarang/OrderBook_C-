#!/usr/bin/env python3
"""
generate_market_data.py

Generates a simulated market-data feed as BOTH:
  - a CSV file (human-readable, small samples committed to the repo), and
  - a binary replay file (the 32-byte-record format read by the
    `matching_engine` pipeline – see include/ob/replay_format.h).

The binary path is what the pipeline should consume for benchmarking:
parsing cost drops to a memcpy-per-event, so the feed handler no longer
dominates pipeline latency (report §6.2).

Design choices (inherited from the original generator):
  - Orders cluster around a mid-price that random-walks (Brownian motion),
    mimicking real quote clustering near the NBBO.
  - Cancellations target recently-added live orders (real cancel rates are
    high, often 90%+).
  - BID prices stay below mid; ASK prices above mid, preserving a spread.

Usage:
  python3 scripts/generate_market_data.py [--n 500000] [--seed 42]
          [--csv out.csv] [--bin out.bin] [--cancel-ratio 0.45]
          [--mid 10000] [--spread 4] [--jitter 10] [--drift 0.5]
"""

import argparse
import random
import struct
import sys
from pathlib import Path

MAGIC = b"OBRPLAY\x00"   # 8 bytes – matches ob::replay::kMagic exactly

# Event record: little-endian, 32 bytes, matches ob::replay::Event.
#   <Q = uint64 order_id, ii = int32 price/qty, Q = uint64 ingest_ts,
#    BB = action/side, H = uint16 flags, i = int32 reserved>
EVENT_STRUCT = struct.Struct("<QiiQBBHi")
ACTION_ADD, ACTION_CANCEL = 0, 1
SIDE_BID, SIDE_ASK = 0, 1


def clamp(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, value))


def generate(n_orders, cancel_ratio, mid_start, spread, jitter, drift_std,
             seed, csv_path, bin_path):
    rng = random.Random(seed)
    mid = mid_start
    next_order_id = 1

    live_orders: dict[int, tuple[str, int, int]] = {}
    live_ids: list[int] = []

    csv_rows: list[tuple] = []
    bin_records: list[bytes] = []

    for _ in range(n_orders):
        mid += int(rng.gauss(0, drift_std))
        mid = clamp(mid, mid_start - 500, mid_start + 500)

        do_cancel = live_ids and rng.random() < cancel_ratio

        if do_cancel:
            idx = rng.randrange(len(live_ids))
            cancel_id = live_ids[idx]
            live_ids[idx] = live_ids[-1]
            live_ids.pop()
            side, price, qty = live_orders.pop(cancel_id)
            csv_rows.append(("C", cancel_id, side, price, qty))
            bin_records.append(EVENT_STRUCT.pack(
                cancel_id, price, qty, 0, ACTION_CANCEL,
                SIDE_BID if side == "BID" else SIDE_ASK, 0, 0))
        else:
            side = "BID" if rng.random() < 0.5 else "ASK"
            if side == "BID":
                base = mid - spread
                price = clamp(base + rng.randint(-jitter, 0), 1, mid_start + 1000)
            else:
                base = mid + spread
                price = clamp(base + rng.randint(0, jitter), 1, mid_start + 1000)

            qty = rng.randint(1, 200)
            order_id = next_order_id
            next_order_id += 1
            live_orders[order_id] = (side, price, qty)
            live_ids.append(order_id)
            csv_rows.append(("A", order_id, side, price, qty))
            bin_records.append(EVENT_STRUCT.pack(
                order_id, price, qty, 0, ACTION_ADD,
                SIDE_BID if side == "BID" else SIDE_ASK, 0, 0))

    if csv_path:
        with open(csv_path, "w", newline="") as fh:
            fh.write("action,order_id,side,price,qty\n")
            for r in csv_rows:
                fh.write(f"{r[0]},{r[1]},{r[2]},{r[3]},{r[4]}\n")

    if bin_path:
        with open(bin_path, "wb") as fh:
            fh.write(MAGIC)
            fh.write(struct.pack("<QQ", len(bin_records), 0))
            fh.write(b"".join(bin_records))

    n_adds = sum(1 for r in csv_rows if r[0] == "A")
    print(f"generated {len(csv_rows):,} rows "
          f"(adds {n_adds:,}, cancels {len(csv_rows) - n_adds:,})")
    if csv_path:
        print(f"  csv -> {csv_path}")
    if bin_path:
        print(f"  bin -> {bin_path} "
              f"({(24 + 32 * len(bin_records)) / 1e6:.1f} MB)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=500_000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--cancel-ratio", type=float, default=0.45)
    ap.add_argument("--mid", type=int, default=10_000)
    ap.add_argument("--spread", type=int, default=4)
    ap.add_argument("--jitter", type=int, default=10)
    ap.add_argument("--drift", type=float, default=0.5)
    ap.add_argument("--csv", default=None, help="output csv path")
    ap.add_argument("--bin", default=None, help="output binary replay path")
    args = ap.parse_args()

    if not args.csv and not args.bin:
        args.csv = "data/market_data.csv"
        args.bin = "data/market_data.bin"

    Path(args.csv).parent.mkdir(parents=True, exist_ok=True) if args.csv else None
    Path(args.bin).parent.mkdir(parents=True, exist_ok=True) if args.bin else None

    generate(args.n, args.cancel_ratio, args.mid, args.spread, args.jitter,
             args.drift, args.seed, args.csv, args.bin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
