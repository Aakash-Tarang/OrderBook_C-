# Generated & transient artifacts

Everything in this folder is **regenerable** and kept out of Git.

| Path | Produced by | Consumed by |
|------|-------------|-------------|
| `market_data.csv`      | `python3 scripts/generate_market_data.py --n 500000 --csv data/market_data.csv` | `matching_engine --input …csv` (slow text path, kept for comparison) |
| `market_data.bin`      | `python3 scripts/generate_market_data.py --n 500000 --bin data/market_data.bin` | `matching_engine --input …bin` (fast binary replay path) |
| `market_data_sample.csv` | `python3 scripts/generate_market_data.py --n 20000 --csv data/market_data_sample.csv` | quick smoke runs |

The 20k-row sample **is committed** (small enough to review); the 500k-row
CSV (~10 MB) and the 16 MB binary replay are not — regenerate them locally
with the exact command above (seed 42, so files are reproducible).
