#!/usr/bin/env bash
# The whole exchange under one batch, with ToyExchange's PerformanceServiceApp listening: the replicas, the
# recorder and the gateway as programs on real sockets, one client sending a batch of orders, then a clear.
# The replicas forward a telemetry trace for every command, and the performance service prints percentiles
# for each second. The output is a directory of logs.
#
#   Bench/telemetry_run.sh                     10,000 bids and 7,500 asks, two replicas and the recorder
#   BIDS=2000 ASKS=1500 Bench/telemetry_run.sh another size
#   REPLICAS=1 Bench/telemetry_run.sh          one replica
#   NUDGE=0 Bench/telemetry_run.sh             do not send the final cancel that lets the replicas recover lost orders
#   AFTER_BIDS=0 AFTER_ASKS=0 Bench/telemetry_run.sh   no orders behind the clear (default 1500 bids, 1000 asks)
#   OUT=run1 Bench/telemetry_run.sh            another output directory (default telemetry_run)
#   BUILD=./build Bench/telemetry_run.sh       where the build is
#
# The performance service is not part of this repository's build. It is configured from the Core source that
# CMake fetched, into $BUILD/perf, the first time this runs. Run it with nothing else using the machine.
set -u

BUILD="$(cd "${BUILD:-./build}" && pwd)"
OUT="${OUT:-telemetry_run}"
BIDS="${BIDS:-10000}"
ASKS="${ASKS:-7500}"
SEED="${SEED:-1}"
REPLICAS="${REPLICAS:-2}"
# Orders sent straight after the clear, for the next batch. The replicas receive them while the clear is running,
# so they wait in the replica's queue, which is what InboundQDepth and QueuingDelayNs measure.
AFTER_BIDS="${AFTER_BIDS:-1500}"
AFTER_ASKS="${AFTER_ASKS:-1000}"
# A replica learns that it missed a command only when a later one arrives, so orders lost at its socket while the
# clear held the queue stay missing until then. With NUDGE=1, one harmless cancel follows the orders and the
# replicas recover the rest. NUDGE=0 leaves the loss in the logs.
NUDGE="${NUDGE:-1}"
SETTLE="${SETTLE:-4}"   # seconds to let the orders drain before the clear, and the clear's window flush after
HERE="$(cd "$(dirname "$0")" && pwd)"

PERF="$BUILD/perf/PerformanceService/PerformanceServiceApp"
if [ ! -x "$PERF" ]; then
  echo "building PerformanceServiceApp from the fetched Core source" >&2
  cmake -S "$BUILD/_deps/core-src" -B "$BUILD/perf" -DCMAKE_BUILD_TYPE=Release > /dev/null &&
    cmake --build "$BUILD/perf" --target PerformanceServiceApp -j > /dev/null || { echo "cannot build it" >&2; exit 1; }
fi
for program in MatchingService/MatchingServiceApp Recorder/RecorderApp NetworkService/NetworkServiceApp Client/BatchClient; do
  [ -x "$BUILD/$program" ] || { echo "missing $BUILD/$program: build the project first" >&2; exit 1; }
done

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cd "$OUT" || exit 1
rm -f network_sequence_store.dat results.sqlite results.sqlite-wal results.sqlite-shm \
      perf.log replica*.log recorder.log gateway.log client.log load.txt after.txt

pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill -INT "$pid" 2> /dev/null; done
  sleep 1
  for pid in "${pids[@]}"; do kill -KILL "$pid" 2> /dev/null; done
}
trap cleanup EXIT

start() {
  local log="$1"; shift
  "$@" > "$log" 2>&1 &
  pids+=($!)
}

"$HERE/gen_load.py" --bids "$BIDS" --asks "$ASKS" --seed "$SEED" > load.txt || exit 1
"$HERE/gen_load.py" --bids "$AFTER_BIDS" --asks "$AFTER_ASKS" --seed "$((SEED + 1))" > after.txt || exit 1

# The performance service first, so no report is sent to nothing. Then the replicas and the recorder, then the
# gateway, which is the order the README gives.
start perf.log "$PERF"
sleep 0.5
for id in $(seq 1 "$REPLICAS"); do start "replica$id.log" "$BUILD/MatchingService/MatchingServiceApp" --replica-id "$id"; done
start recorder.log "$BUILD/Recorder/RecorderApp" --db results.sqlite
sleep 1
start gateway.log "$BUILD/NetworkService/NetworkServiceApp" --no-ticker
sleep 1.5

echo "sending $(grep -c '^\(bid\|ask\)' load.txt) orders, a clear, and $(grep -c '^\(bid\|ask\)' after.txt) more behind it" >&2
start_ns=$(date +%s%N)
{
  cat load.txt
  echo "wait $SETTLE"
  echo "clear"
  cat after.txt
  echo "wait $SETTLE"
  if [ "$NUDGE" = 1 ]; then echo "cancel p=1 t=1"; echo "wait $SETTLE"; fi
  echo "quit"
} | "$BUILD/Client/BatchClient" --batch 0 > client.log 2>&1
echo "client exit $?, $(( ($(date +%s%N) - start_ns) / 1000000 )) ms in all" >&2
sleep 1

echo "--- the client saw" >&2
for type in ACCEPTED REJECTED FILL ORDER_DONE BATCH_DONE BATCH_VOIDED; do
  printf '%-13s %s\n' "$type" "$(grep -c "^$type " client.log)" >&2
done
echo "(a clear that emits more than about 240 events loses the end of them on the way to the client: see Known limits)" >&2

echo "--- the recorder wrote" >&2
python3 - results.sqlite >&2 << 'PY'
import sqlite3, sys
db = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
for row in db.execute("select batch_id, status, void_reason, bids, asks, arcs, pivots, units, lifted_units, welfare, "
                      "fills, certified from batches order by batch_id"):
    print(dict(zip(("batch", "status", "void", "bids", "asks", "arcs", "pivots", "units", "lifted", "welfare",
                    "fills", "certified"), row)))
PY

echo "--- $OUT/perf.log" >&2
grep -c '^\[perf\]' perf.log >&2
