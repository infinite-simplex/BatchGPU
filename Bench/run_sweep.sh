#!/usr/bin/env bash
# Every (solver, price range, batch size) runs as its own process, and one that runs longer than 30
# seconds is killed and reported as "> 30 s". Every size runs, including those above a size that
# already timed out.
#
#   Bench/run_sweep.sh                      all solvers the benchmark knows
#   SOLVERS="ssp" Bench/run_sweep.sh        one solver
#   BIDS="100 500" Bench/run_sweep.sh       other sizes
#   LIMIT=60 Bench/run_sweep.sh             another limit, in seconds
#   GEN=realistic Bench/run_sweep.sh        ERCOT-calibrated batches (PRICES is ignored)
#   BENCH=./build/ClearingBench Bench/run_sweep.sh
#
# Output is CSV on stdout. Batch shape for a size of N bids:
#   legacy     N/5 asks, N/2 items, each ask offering 5 items and each bid accepting ACCEPT items.
#   realistic  3N/4 asks (ERCOT's median ratio), N/4 items (ERCOT's bids per location), each ask
#              offering OFFER items and each bid accepting ACCEPT items.
# Run one sweep at a time and never during a build, or the timings are not comparable.
set -u

BENCH="${BENCH:-./build/ClearingBench}"
LIMIT="${LIMIT:-30}"
BIDS="${BIDS:-100 500 1000 2000 5000 10000}"
PRICES="${PRICES:-60 2000 100000}"
ACCEPT="${ACCEPT:-20}"
OFFER="${OFFER:-5}"
GEN="${GEN:-legacy}"
if [ "$GEN" = "realistic" ]; then PRICES="-"; fi
SOLVERS="${SOLVERS:-$("$BENCH" --help | sed -n '/^solvers:/,$p' | tail -n +2 | tr -d ' ' | tr '\n' ' ')}"

echo "solver,max_price,items,asks,bids,offer_per_ask,accept_per_bid,nodes,arcs,steps,traded_units,clear_ms_median,clear_ms_p95,build_ms,solve_ms,price_ms"
for solver in $SOLVERS; do
  for price in $PRICES; do
    for bids in $BIDS; do
      if [ "$GEN" = "realistic" ]; then
        asks=$((bids * 3 / 4))
        items=$((bids / 4))
        maxprice_args=""
      else
        asks=$((bids / 5))
        items=$((bids / 2))
        maxprice_args="--maxprice $price"
      fi
      runs=1
      if [ "$bids" -le 1000 ]; then runs=3; fi
      row=$(timeout --signal=KILL "$LIMIT" "$BENCH" --solver "$solver" --gen "$GEN" $maxprice_args \
              --runs "$runs" --no-header --one "$items" "$asks" "$bids" "$OFFER" "$ACCEPT")
      status=$?
      if [ $status -eq 124 ] || [ $status -eq 137 ]; then
        echo "$solver,$price,$items,$asks,$bids,$OFFER,$ACCEPT,,,,,> ${LIMIT} s,,,,"
      elif [ $status -eq 3 ]; then
        # ClearingBench's EXIT_REJECTED
        echo "$solver,$price,$items,$asks,$bids,$OFFER,$ACCEPT,,,,,rejected (over a limit),,,,"
      elif [ $status -eq 4 ]; then
        echo "$solver,$price,$items,$asks,$bids,$OFFER,$ACCEPT,,,,,over the pivot budget,,,,"
      elif [ $status -ne 0 ]; then
        echo "$solver,$price,$items,$asks,$bids,$OFFER,$ACCEPT,,,,,FAILED (exit $status),,,," >&2
      else
        echo "$row"
      fi
    done
  done
done
