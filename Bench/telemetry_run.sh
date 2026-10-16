#!/usr/bin/env bash
# The whole exchange under one batch, with ToyExchange's PerformanceServiceApp listening: the replicas, the
# recorder and the gateway as programs on real sockets, one client sending a batch of orders, then a clear.
# The replicas forward a telemetry trace for every command, and the performance service prints percentiles
# for each second. The output is a directory of logs.
#
#   Bench/telemetry_run.sh                     10,000 bids and 7,500 asks, two replicas and the recorder
#   BIDS=2000 ASKS=1500 Bench/telemetry_run.sh another size
#   REPLICAS=1 Bench/telemetry_run.sh          one replica
#   TUNED=1 Bench/telemetry_run.sh             Core's load settings (spinning receivers, no console echo)
#   RECORDERS=2 Bench/telemetry_run.sh         two recorders, whose digests are compared at the end
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
# TUNED=1 uses the settings Core v0.3.0 recommends for load: receivers that spin for 100 us after each datagram, and
# no console line per event on the gateway. Without it the programs run on Core's defaults.
TUNED="${TUNED:-0}"
RECORDERS="${RECORDERS:-1}"   # each writes its own results file, and the summary compares them
# Orders sent straight after the clear, for the next batch. The replicas receive them while the clear is running,
# so they wait in the replica's queue, which is what InboundQDepth and QueuingDelayNs measure. The queue holds 1,024
# commands, so a burst this size overflows the replicas' socket buffers too, and what they miss they fetch from the
# gateway after 200 ms without a datagram (Core v0.2.0). The summary shows whether every order was recorded.
AFTER_BIDS="${AFTER_BIDS:-1500}"
AFTER_ASKS="${AFTER_ASKS:-1000}"
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
rm -f network_sequence_store.dat results*.sqlite results*.sqlite-wal results*.sqlite-shm \
      perf.log replica*.log recorder*.log gateway.log client.log load.txt after.txt

# The kernel counts UDP datagrams it dropped because a socket's receive buffer was full, for the whole machine.
rcvbuf_errors() {
  awk '/^Udp:/ { if (!seen) { for (i = 1; i <= NF; i++) if ($i == "RcvbufErrors") column = i; seen = 1 } else print $column }' /proc/net/snmp
}

replica_flags=(); gateway_flags=(--no-ticker)
# SESSION_BUFFER=<bytes> raises how much unsent output the gateway keeps for the one client (default 1 MiB).
[ -n "${SESSION_BUFFER:-}" ] && gateway_flags+=(--session-buffer-bytes "$SESSION_BUFFER")
if [ "$TUNED" = 1 ]; then replica_flags=(--ingress-spin-micros 100); gateway_flags+=(--no-console-echo); fi

pids=()

# Another exchange on this machine shares the ports, the multicast groups and the CPU, which breaks a run or skews
# its times. Programs of this run are in pids; any other process of these names is someone else's.
foreign_exchange() {
  local own
  own=$(printf '%s
' "${pids[@]+"${pids[@]}"}" | grep -v '^$')
  pgrep -f 'MatchingServiceApp|NetworkServiceApp|RecorderApp|PerformanceServiceApp' |
    grep -vxF -f <(printf '%s
' "$own" "$$") || true
}
if [ -n "$(foreign_exchange)" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "another exchange is already running on this machine (pids: $(foreign_exchange | tr '
' ' ')): not starting." >&2
  echo "Stop it, or set FORCE=1 to run anyway." >&2
  exit 1
fi

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
for id in $(seq 1 "$REPLICAS"); do start "replica$id.log" "$BUILD/MatchingService/MatchingServiceApp" --replica-id "$id" ${replica_flags[@]+"${replica_flags[@]}"}; done
for id in $(seq 1 "$RECORDERS"); do
  start "recorder$id.log" "$BUILD/Recorder/RecorderApp" --db "results$id.sqlite" ${replica_flags[@]+"${replica_flags[@]}"}
done
sleep 1
start gateway.log "$BUILD/NetworkService/NetworkServiceApp" "${gateway_flags[@]}"
sleep 1.5

echo "sending $(grep -c '^\(bid\|ask\)' load.txt) orders, a clear, and $(grep -c '^\(bid\|ask\)' after.txt) more behind it" >&2
start_ns=$(date +%s%N)
dropped_before=$(rcvbuf_errors)
{
  cat load.txt
  echo "wait $SETTLE"
  echo "clear"
  cat after.txt
  echo "wait $SETTLE"
  echo "quit"
} | "$BUILD/Client/BatchClient" --batch 0 > client.log 2>&1
echo "client exit $?, $(( ($(date +%s%N) - start_ns) / 1000000 )) ms in all" >&2
sleep 1
[ -z "$(foreign_exchange)" ] || echo "WARNING: another exchange was running during this run: its times and counts are not clean" >&2
echo "--- UDP datagrams dropped for a full receive buffer during the run (whole machine): $(( $(rcvbuf_errors) - dropped_before ))" >&2

echo "--- the client saw" >&2
for type in ACCEPTED REJECTED FILL ORDER_DONE BATCH_DONE BATCH_VOIDED; do
  printf '%-13s %s\n' "$type" "$(grep -c "^$type " client.log)" >&2
done
echo "--- the gateway's own count (events from the replicas, and output it had to drop for the client)" >&2
grep "NetworkService egress:" gateway.log >&2 || echo "(not printed)" >&2
grep "did not receive" gateway.log | tail -1 >&2 || true

echo "--- the recorders wrote" >&2
python3 - results*.sqlite >&2 << 'PY'
import sqlite3, sys
digests = []
for path in sorted(sys.argv[1:]):
    if path.endswith(("-wal", "-shm")):
        continue
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    print(path)
    rows = db.execute("select batch_id, status, void_reason, bids, asks, arcs, pivots, units, lifted_units, welfare, "
                      "fills, certified, hex(digest) from batches order by batch_id").fetchall()
    for row in rows:
        print("  batch", dict(zip(("id", "status", "void", "bids", "asks", "arcs", "pivots", "units", "lifted",
                                   "welfare", "fills", "certified"), row[:-1])), "digest", row[-1][:16])
    print("  orders per batch", db.execute("select batch_id, count(*) from orders group by 1 order by 1").fetchall(),
          " open orders", db.execute("select count(*) from open_orders").fetchone()[0])
    digests.append([(r[0], r[-1]) for r in rows])
print("recorders agree on every digest:", all(d == digests[0] for d in digests))
PY

echo "--- $OUT/perf.log" >&2
grep -c '^\[perf\]' perf.log >&2
