import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

# ---------------------------------------------------
# Load CSV
# ---------------------------------------------------
df_full = pd.read_csv("latency.csv")

# ---------------------------------------------------
# ORIGINAL METRICS (before filtering)
# ---------------------------------------------------
raw_latencies = df_full["latency_us"]

raw_p50 = np.percentile(raw_latencies, 50)
raw_p90 = np.percentile(raw_latencies, 90)
raw_p99 = np.percentile(raw_latencies, 99)

raw_mean = np.mean(raw_latencies)
raw_min = np.min(raw_latencies)
raw_max = np.max(raw_latencies)

print("\n========== RAW METRICS ==========")
print("Samples :", len(raw_latencies))
print("MIN      :", raw_min, "us")
print("MAX      :", raw_max, "us")
print("MEAN     :", raw_mean, "us")
print("P50      :", raw_p50, "us")
print("P90      :", raw_p90, "us")
print("P99      :", raw_p99, "us")

# ---------------------------------------------------
# Remove warmup period (first 30%)
# ---------------------------------------------------
cutoff = int(len(df_full) * 0.30)

df = df_full.iloc[cutoff:].reset_index(drop=True)

# ---------------------------------------------------
# Remove huge outliers using percentile clipping
# Keep only values below 99.9th percentile
# ---------------------------------------------------
upper_limit = np.percentile(df["latency_us"], 99.9)

df = df[df["latency_us"] <= upper_limit]

latencies = df["latency_us"]

# ---------------------------------------------------
# FILTERED METRICS
# ---------------------------------------------------
p50 = np.percentile(latencies, 50)
p90 = np.percentile(latencies, 90)
p99 = np.percentile(latencies, 99)

mean_latency = np.mean(latencies)

min_latency = np.min(latencies)
max_latency = np.max(latencies)

print("\n========== FILTERED METRICS ==========")
print("Samples :", len(latencies))
print("MIN      :", min_latency, "us")
print("MAX      :", max_latency, "us")
print("MEAN     :", mean_latency, "us")
print("P50      :", p50, "us")
print("P90      :", p90, "us")
print("P99      :", p99, "us")

# ---------------------------------------------------
# Create 2x2 grid
# ---------------------------------------------------
fig, axs = plt.subplots(2, 2, figsize=(14, 10))

# ---------------------------------------------------
# 1. Histogram
# ---------------------------------------------------
axs[0, 0].hist(latencies, bins=40)

axs[0, 0].set_title("Latency Distribution")
axs[0, 0].set_xlabel("Latency (us)")
axs[0, 0].set_ylabel("Frequency")

# ---------------------------------------------------
# 2. Time Series Plot
# ---------------------------------------------------
axs[0, 1].plot(df["iteration"], latencies)

axs[0, 1].set_title("Latency Over Time")
axs[0, 1].set_xlabel("Iteration")
axs[0, 1].set_ylabel("Latency (us)")

# ---------------------------------------------------
# 3. CDF Plot
# ---------------------------------------------------
sorted_latency = np.sort(latencies)

cdf = np.arange(len(sorted_latency)) / float(len(sorted_latency))

axs[1, 0].plot(sorted_latency, cdf)

axs[1, 0].set_title("Latency CDF")
axs[1, 0].set_xlabel("Latency (us)")
axs[1, 0].set_ylabel("CDF")

# ---------------------------------------------------
# 4. Percentile Visualization
# ---------------------------------------------------
percentiles = ["P50", "P90", "P99"]
values = [p50, p90, p99]

axs[1, 1].bar(percentiles, values)

axs[1, 1].set_title("Latency Percentiles")
axs[1, 1].set_ylabel("Latency (us)")

# ---------------------------------------------------
# Layout
# ---------------------------------------------------
plt.tight_layout()

# ---------------------------------------------------
# Save high-quality image
# ---------------------------------------------------
plt.savefig("latency_analysis.png", dpi=300)

# ---------------------------------------------------
# Display
# ---------------------------------------------------
plt.show()