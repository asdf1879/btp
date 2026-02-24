#!/usr/bin/env bash
set -euo pipefail

# Sender benchmark harness for DPDK app.
# Runs the sender multiple times and summarizes duration/throughput statistics.

RUNS=50
BIN="./build/receiver"
PORT=0
PEER_MAC=""
SRC_IP=""
DST_IP=""
COUNT=4000
EXTRA_EAL="-l 1-2 -n 4"

usage() {
  cat <<USAGE
Usage: $0 --peer-mac <mac> --src-ip <ip> --dst-ip <ip> [options]

Required:
  --peer-mac <mac>       Receiver MAC address
  --src-ip <ip>          Source IPv4 used in test packet header
  --dst-ip <ip>          Destination IPv4 used in test packet header

Options:
  --runs <n>             Number of runs (default: 50)
  --count <n>            Packets per run (default: 4000)
  --port <id>            DPDK port id on sender (default: 0)
  --bin <path>           Sender binary path (default: ./build/receiver)
  --eal "<args>"         EAL args string (default: "-l 1-2 -n 4")
  --help                 Show this help

Example:
  $0 --peer-mac a0:36:9f:2a:5d:28 --src-ip 192.168.100.1 --dst-ip 192.168.100.2 --runs 50 --count 4000
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs) RUNS="$2"; shift 2 ;;
    --count) COUNT="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --eal) EXTRA_EAL="$2"; shift 2 ;;
    --peer-mac) PEER_MAC="$2"; shift 2 ;;
    --src-ip) SRC_IP="$2"; shift 2 ;;
    --dst-ip) DST_IP="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "$PEER_MAC" || -z "$SRC_IP" || -z "$DST_IP" ]]; then
  echo "Error: --peer-mac, --src-ip, and --dst-ip are required." >&2
  usage
  exit 1
fi

if [[ ! -x "$BIN" ]]; then
  echo "Error: binary not found or not executable: $BIN" >&2
  exit 1
fi

ts="$(date +%Y%m%d_%H%M%S)"
out_dir="bench_results_${ts}"
mkdir -p "$out_dir"
raw_csv="$out_dir/raw.csv"
summary_txt="$out_dir/summary.txt"

printf "run,duration_s,throughput_pps,sent,received,lost\n" > "$raw_csv"

echo "Running $RUNS sender benchmarks..."
echo "Results dir: $out_dir"

for ((i=1; i<=RUNS; i++)); do
  log_file="$out_dir/run_${i}.log"

  # shellcheck disable=SC2086
  sudo "$BIN" $EXTRA_EAL -- \
    --mode=tx --port="$PORT" \
    --peer-mac="$PEER_MAC" \
    --src-ip="$SRC_IP" --dst-ip="$DST_IP" \
    --count="$COUNT" > "$log_file" 2>&1 || true

  perf_line="$(grep -E '^Perf:' "$log_file" | tail -n1 || true)"
  done_line="$(grep -E '^Completed:' "$log_file" | tail -n1 || true)"

  duration=""
  tput=""
  sent=""
  recv=""
  lost=""

  if [[ -n "$perf_line" ]]; then
    duration="$(echo "$perf_line" | sed -nE 's/.*duration=([0-9.]+) s.*/\1/p')"
    tput="$(echo "$perf_line" | sed -nE 's/.*throughput=([0-9.]+) pps.*/\1/p')"
  fi

  if [[ -n "$done_line" ]]; then
    sent="$(echo "$done_line" | sed -nE 's/.*sent=([0-9]+)\/.*/\1/p')"
    recv="$(echo "$done_line" | sed -nE 's/.*received=([0-9]+)\/.*/\1/p')"
    lost="$(echo "$done_line" | sed -nE 's/.*lost=([0-9]+).*/\1/p')"
  fi

  printf "%d,%s,%s,%s,%s,%s\n" "$i" "$duration" "$tput" "$sent" "$recv" "$lost" >> "$raw_csv"

  if [[ -n "$duration" && -n "$tput" ]]; then
    echo "[$i/$RUNS] duration=${duration}s throughput=${tput}pps sent=${sent:-?} recv=${recv:-?} lost=${lost:-?}"
  else
    echo "[$i/$RUNS] missing Perf line (check $log_file)"
  fi

done

python3 - "$raw_csv" "$summary_txt" <<'PY'
import csv
import statistics
import sys

raw_csv = sys.argv[1]
summary_txt = sys.argv[2]

runs = []
durations = []
throughputs = []
losses = []

with open(raw_csv, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        runs.append(row)
        if row["duration_s"]:
            durations.append(float(row["duration_s"]))
        if row["throughput_pps"]:
            throughputs.append(float(row["throughput_pps"]))
        if row["lost"]:
            losses.append(int(row["lost"]))

def pct(values, p):
    if not values:
        return float("nan")
    vals = sorted(values)
    if len(vals) == 1:
        return vals[0]
    k = (len(vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(vals) - 1)
    if f == c:
        return vals[f]
    return vals[f] + (vals[c] - vals[f]) * (k - f)

def fmt(x):
    return f"{x:.6f}"

ok = len(durations)
total = len(runs)
loss_total = sum(losses) if losses else 0

lines = []
lines.append(f"total_runs={total}")
lines.append(f"successful_runs_with_perf={ok}")
lines.append(f"total_lost_packets_reported={loss_total}")

if ok:
    lines.append("duration_s:")
    lines.append(f"  mean={fmt(statistics.mean(durations))}")
    lines.append(f"  median={fmt(statistics.median(durations))}")
    lines.append(f"  p90={fmt(pct(durations, 0.90))}")
    lines.append(f"  p95={fmt(pct(durations, 0.95))}")
    lines.append(f"  p99={fmt(pct(durations, 0.99))}")

    lines.append("throughput_pps:")
    lines.append(f"  mean={fmt(statistics.mean(throughputs))}")
    lines.append(f"  median={fmt(statistics.median(throughputs))}")
    lines.append(f"  p90={fmt(pct(throughputs, 0.90))}")
    lines.append(f"  p95={fmt(pct(throughputs, 0.95))}")
    lines.append(f"  p99={fmt(pct(throughputs, 0.99))}")

text = "\n".join(lines)
print(text)

with open(summary_txt, "w") as f:
    f.write(text + "\n")
PY

echo ""
echo "Summary written to: $summary_txt"
echo "Raw per-run data:   $raw_csv"
