# BatchGPU

[![CI](https://github.com/infinite-simplex/BatchGPU/actions/workflows/ci.yml/badge.svg)](https://github.com/infinite-simplex/BatchGPU/actions/workflows/ci.yml)

A GPU compute exchange that clears in hourly batches. A bid names what it can
accept (a set of GPU models, interconnects, regions and hours), so a buyer who
would take an A100 or an H100 can be served by either, and the batch clears
every item at one minimal price. It is built on
[ToyExchange](https://github.com/infinite-simplex/ToyExchange)'s replicated,
leader-elected pipeline (Core v0.3.0, pinned in `CMakeLists.txt`) via CMake
`FetchContent`, the same way
[EventContractLOB](https://github.com/infinite-simplex/EventContractLOB) is.

This is a portfolio project, not a production exchange. It clears and records; it
does not settle, deliver, verify hardware or take payment.

Design rationale and tradeoffs are written up in the
[BatchGPU design write-up](https://infinite-simplex.github.io/InfiniteLabs/#BatchGPU).

## Status

It runs end to end. The **clearing module** turns a batch of orders into fills and prices.
The **order protocol** is the catalog, the command and event types, and the protocol handler
with its stateless checks. The **batch engine** holds the open batch, makes the checks that
need its state, enforces the buyers' quotas, and runs the clear through to the events. Around
them are the programs: the **gateway** with its ticker, the **replica**, the **recorder**,
which writes each batch's results to a SQLite file, and the **results API**, a read-only HTTP
server over the recorders' files: a participant's results from the clears, the clearing price
of each GPU or TPU model, and, when it is asked to, the orders that wait for the next clear.
One test starts them all on real sockets and checks that the clients, the public feed, the
database and the API agree.

The clearing module runs network simplex. On one machine it clears 10,000 bids and 7,500
asks in 0.02 to 1.1 seconds on order books shaped like ERCOT's, and batches built to be hard
in 0.3 to 3.0 seconds (`Bench/` reproduces the sweep). Every clear ends with an optimality
certificate, and the work it may do is bounded by an arc limit and a pivot budget (1,000,000
arcs and 4,000,000 pivots). Successive shortest paths is kept as an independent reference
for the tests.

## What is here

- `Clearing/`: a pure function from orders to fills and prices. An integer
  min-cost flow, solved by network simplex, maximizes welfare, then trades the most
  units, then favors the orders that arrived first. Prices are the minimal optimal
  price vector. The same input always gives the same result, which is what lets
  replicas agree, and a clear proves its answer optimal before returning it.
  `clearReference()` solves the same problem by successive shortest paths, for the
  tests to compare against.
- `ExchangeCommon/`: the type aliases and every limit, in one place
  (`Limits.hpp`), which every replica must agree on because the limits decide which orders are
  accepted. That includes the per-buyer quotas (at most 500 buyers per batch), which the engine
  enforces. Also the catalog of accelerators, interconnects and regions (`Catalog.hpp`),
  the command and event types (`BatchCommand.hpp`, `BatchEvent.hpp`, `RejectReason.hpp`),
  the stateless checks on an order (`OrderChecks.hpp`), and the schedule that turns a
  batch id into the hours it sells (`Schedule.hpp`).
- `NetworkService/`: the protocol handler, `BatchProtocolHandler`, which Core's gateway
  calls to turn a client's bytes into a command (`BatchWire.hpp` has the frame layout, and
  every frame, in both directions, starts with a protocol version byte). It closes a
  connection on a frame that cannot be read, one of another version included, and turns an
  order that fails a check into an `INVALID` command that says why. `BatchEventFrame.hpp` is the frame that clients get
  back, privately for their own orders and on a public feed for everyone. `BatchTicker` is the
  exchange's clock: it sends `BATCH_CLEAR` to the gateway when a clear is due. `NetworkServiceApp`
  is the gateway program: order entry, the sequenced log and its retransmit server, the replicas'
  arbiter, the results going back out, and the ticker.
- `MatchingService/`: the batch engine, `BatchEngine`, which Core's replica host hands every
  command to. `BatchBook` keeps the open batch and decides who gets in (the token, the hours,
  a buyer's quota, the room in the batch). On `BATCH_CLEAR` the engine runs the clear and emits
  every fill, every order's result and the batch's, or a single event if the batch cannot be
  cleared. No event depends on a clock or on randomness, so replicas emit the same events.
  `MatchingServiceApp` is a replica: it joins the gateway's log, runs the engine, publishes
  the results while it is the leader, and forwards one telemetry trace for each command, as
  Core's replicas do: the delays and the queue (Core's stamps), the time inside the engine, and,
  on the clear's trace, the lifted units, the bids filled and unfilled, and the solver's pivots.
- `Recorder/`: the recorder, `RecorderApp`, a replica that never leads (it sends no heartbeats
  and publishes nothing). It takes the same log as every replica, runs the same engine, and
  writes each batch to a SQLite file in one transaction: `batches` (one row each, with a
  SHA-256 digest of the results), `orders` (each with what it asked for and what it got, and
  not the ones that were cancelled), `fills`, and `accelerator_prices` (one row for each GPU
  or TPU model and hour that traded: the units, the money, and the cheapest and dearest item
  price, never a price for each interconnect and region). The first record of a batch id wins,
  so a replay changes nothing. The orders of the batch that is still open are kept in two more
  tables, written every 50 ms and left out of the digest. SQLite is the official amalgamation,
  fetched when CMake configures.
- `ResultsApi/`: `ResultsApiApp`, a separate process that opens each recorder's file read-only
  and serves the results as JSON over HTTP, GET only. It has its own small HTTP server, with
  nothing fetched. Batches (and which recorders hold them, and whether they agree), the orders,
  fills and prices of a batch in pages, one order with its fills, one participant's orders
  across batches, the price of a model over the batches, and, only with `--open-orders`, a
  participant's orders in the open batch. Every answer says how far each recorder has got. It
  listens on 127.0.0.1 and has no authentication.
- `Client/`: `BatchClient`, a command-line order-entry client. It builds its frames with the
  encoders in `BatchWire.hpp` and reads the events with `BatchEventFrame.hpp`, and takes the
  catalog and the schedule from the same headers the exchange uses. It is not covered by the tests.
- `Tests/`: 59 tests, run by `ctest`. Each checks a behavior or a property over a table or a
  stream of cases, not a single case.
  - Clearing: hand-worked batches and tie-breaks, the input it must refuse, every solver
    against 162 recorded answers (`Tests/fixtures/`, written by an independent Python oracle
    that is no longer in the repository), against the reference solver on bigger batches and
    on 4,000 tiny random ones, golden outputs for fixed batches, and the optimality
    certificate against every kind of wrong flow.
  - Protocol and engine: the catalog, every stateless check at its boundary and in its order,
    the handler's framing and garbage input, the book's every reject, the worked example end
    to end, the voids, Core's real replica host running the engine, and a separately written
    model of the engine that thousands of random commands must agree with on every event.
  - Recorder: the digest, the store (a changed row noticed, the first record of an id kept, a
    failed write leaves nothing), two recorders that agree, a replay that changes nothing, the
    open batch, and the real program fed a log over UDP.
  - The whole exchange: the gateway, two replicas, the recorder and the API as programs on
    real sockets, four clients sending orders, cancels and clears, with every event compared
    across the clients, the public feed, the database and the API against the engine run in
    process on the same bytes, and the telemetry that each replica forwards.
  - The API: the HTTP reader, the JSON writer, every endpoint against the files it serves, and
    clients that go wrong (malformed, oversized, stalled, many at once).
- `Tools/Mutation/`: `plants.py` lists 46 one-line changes to the source, each of which some
  test must notice, and `mutate.py` plants them one at a time in a private copy of the tree,
  rebuilds, runs every test and reports any that no test caught. The 13 newest tests (the
  recorder, the event frame, the ticker, the whole exchange and the API) have no plants.
- `Support/`: batch generators. `SyntheticBatch.hpp` is uniform and integer-only.
  `RealisticBatch.hpp` is calibrated to five days of ERCOT's day-ahead order books, a real
  batch auction with curve bids. `WindowBatch.hpp` has the item structure of this exchange:
  windows of hourly slots.
- `Bench/`: `ClearingBench` shows how clear time grows with batch size for the production
  solver and the reference, on uniform, ERCOT-shaped and exchange-shaped batches and on
  batches built to be hard (a few hot items, every price tied, long chains, wide accept
  sets). It lets you change the production solver's arc limit and pivot budget.
  `run_sweep.sh` runs every cell as its own process under a 30 s kill. `telemetry_run.sh` runs
  the whole exchange under one batch with ToyExchange's `PerformanceServiceApp` listening, and
  `gen_load.py` writes the orders it sends through the client.

## Building and running

Linux only, because Core uses epoll, mmap and multicast sockets. On Windows,
build in WSL. The first configure fetches Core, googletest and SQLite 3.53.4 (about 3 MB,
checked against the hash that sqlite.org publishes).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
Bench/run_sweep.sh                 # every cell killed after 30 s, reported as "> 30 s"
GEN=realistic Bench/run_sweep.sh   # the same sweep on ERCOT-calibrated batches
Bench/telemetry_run.sh             # 10,000 bids and 7,500 asks through the whole exchange, with telemetry
./build/ClearingBench --solver ns --gen dense --one 500 10000 10000 47 50   # a batch built to be hard
```

To run the exchange, start the replicas and the recorder, and then the gateway (Ctrl+C stops
each). A replica or recorder that starts after the gateway catches up from the gateway's log on
its own, but all of them must be restarted if the gateway is: they take a restarted gateway's
commands for duplicates and ignore them. The gateway takes orders on port 10001 and
its ticker sends a clear as soon as it starts and then one at half past each hour; `--no-ticker`
leaves the clears to a client. Every flag has a default for one machine, and the options are in
each `main.cpp`. For load, Core recommends receivers that spin (`--ingress-spin-micros 100`
on the replicas and the recorder) and no console line per event (`--no-console-echo` on the
gateway), with `net.core.rmem_max` at 4 MiB or more on the host; `TUNED=1 Bench/telemetry_run.sh`
runs with those settings. The replicas, the recorder and the gateway must all be built against the
same Core version.

```bash
build/MatchingService/MatchingServiceApp --replica-id 1
build/MatchingService/MatchingServiceApp --replica-id 2     # optional: only the leader publishes
build/Recorder/RecorderApp --db results.sqlite
build/NetworkService/NetworkServiceApp
```

`Client/BatchClient` is a small order-entry client: type commands, or pipe a script in, and it
prints every event that comes back. It stays connected from the order to the clear, because the
results go to the connection that sent the order. To see a batch clear without waiting for the
half hour, start the gateway with `--no-ticker` and run the demo, which enters an ask and three
bids and clears them (`help` lists the commands, `catalog` the names and the hours on sale):

```bash
build/NetworkService/NetworkServiceApp --no-ticker
build/Client/BatchClient --batch 0        # then type: demo
```

With the ticker on, leave out `--batch`: the open batch follows the clock, as the ticker's clears
do, and the client's orders are cleared at the next half hour. The frames the client sends are in
`NetworkService/BatchWire.hpp`, and the frames it gets back in `NetworkService/BatchEventFrame.hpp`.

The replicas also send one telemetry report for every command to 127.0.0.1:40002
(`--performance-service-ip`, `--performance-service-port`). Nothing in this repository listens.
ToyExchange's `PerformanceServiceApp`, which is built in that repository, does, and prints the
delays, the queue depth, the time inside the engine and the clear's statistics as percentiles
for each second.

The API reads the recorders' files. Give it one `--db` for each recorder, and it answers from
whichever has the batch. It listens on 127.0.0.1:8080 (`--bind`, `--port`, `--workers`), and has
no authentication, so do not bind it beyond the machine unless every participant's orders may be
read by whoever can reach it. The orders that wait for the next clear are bids and asks that have
not cleared, so they are not served at all unless it is started with `--open-orders`, and then
anyone who can reach it can read every one.

```bash
build/ResultsApi/ResultsApiApp --db results.sqlite
curl localhost:8080/status
curl localhost:8080/batches?limit=5
curl localhost:8080/batches/205/orders
curl localhost:8080/batches/205/prices                      # one price for each model and hour
curl 'localhost:8080/prices?accelerator=H100-80GB&lead=0'   # the first hour of each batch, batch by batch
curl localhost:8080/participants/101/orders
curl localhost:8080/participants/101/open_orders            # 403 unless started with --open-orders
```

To check that the tests can fail, plant the bugs in `Tools/Mutation/plants.py` one at a time
(it works on copies of the tree, builds each, and takes a while):

```bash
python3 Tools/Mutation/mutate.py --workers 4
```

## Known limits

Replication, leader election and failover belong to Core, so their limits are listed in the
[ToyExchange](https://github.com/infinite-simplex/ToyExchange) repository. These are BatchGPU's own.

- **No authentication.** The gateway takes the participant id an order claims, and accepts a
  `BATCH_CLEAR` with any batch id from any connection, so anyone who can reach it can trade as
  anyone and trigger or repeat a clear. The results API has none either: it listens on
  127.0.0.1, and the open-orders route is off unless it is started with `--open-orders`.
- **A restart loses the open batch.** The gateway's sequenced log does not survive a restart,
  so the orders waiting for a clear are lost, and the replicas and recorders must be restarted
  with it. A replica or recorder started or restarted alone catches up from the log, but it
  replays from the first command, so only while the gateway's ring (1,048,576 commands) still
  holds it. A new solver build is therefore a planned down window right after a clear. The
  results stay in the recorders' files.
- **Results to clients are best effort.** The replicas reach the gateway over UDP with no
  gap-fill, so a client can miss some of a large clear's results. Across ten telemetry runs of a clear that
  emits about 78,000 events, one client received every event in three and only part in seven; it
  always had the first 17,500 acknowledgements. The gateway's own counters showed no event
  missing from the replicas and no client output dropped, yet it had delivered only 29,560 to
  74,278 of the events, so Core's statistics do not show this loss. It is not the gateway's 1 MiB
  limit on a client's unsent output: 16 MiB changed nothing in three runs. The cause is not
  known. Every result is in the recorder's database and the API, which is where a client asks for
  one it missed.
- **Orders behind a clear wait for it.** A replica's inbound queue holds 1,024 commands and the
  clear holds it while it runs. In the telemetry runs (`Bench/telemetry_run.sh`) 2,500 orders sent
  right behind the clear fill the queue, and the receiver stops reading until the clear ends, so
  they wait in its socket buffer. With Core v0.3.0's 4 MiB buffer they fit: in all ten runs every
  order was recorded and two recorders wrote identical digests (`RECORDERS=2`). The median order
  reached the engine 0.44 to 0.51 s after the gateway took it. The kernel still dropped 75 to
  1,988 UDP datagrams during those runs, counted for the whole machine, and a receiver fetches
  what it missed from the gateway after 200 ms without a datagram. No run has sent a clear while
  a burst was still being recovered.
- **Telemetry is best effort, and Core's histogram has a range.** The performance service's
  histogram for the time inside the engine stops at about 1.07 s, and its counters at
  1,000,000. A longer clear or a larger count is dropped there, without a count.
- **The results file is never pruned.**
- **Fixed limits.** 10,000 live orders and 100,000 submissions a side, 500 buyers a batch (20
  open bids, 200 submissions and 980 arcs each), 1,000,000 arcs and 4,000,000 pivots a clear.
  A batch that runs out of pivots is voided: no fills, and every order lapses. Orders live for
  one batch, and a filled bid is final.
- **Linux only.**

## License

MIT. See [LICENSE](LICENSE).
