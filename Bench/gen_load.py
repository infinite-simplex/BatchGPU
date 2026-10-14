#!/usr/bin/env python3
"""Writes a script for Client/BatchClient: one batch of orders in the shape of the headline benchmark.

Each ask offers one SKU for 5 hours (5 items) and each bid accepts 4 SKUs for the same 5 hours (20 items), as in
"ERCOT-shaped, 5 offered, 20 accepted". Buyers are 500 participants with 20 bids each, which is the most the
book allows. The same seed gives the same orders.

    Bench/gen_load.py --bids 10000 --asks 7500 > load.txt
"""
import argparse
import random
import sys

HOURS = 5
BUYERS = 500
BIDS_PER_BUYER = 20
SELLERS = 100
BUYER_BASE = 2000
SELLER_BASE = 1000

# name, vendor, a typical price in ticks ($0.01) per node-hour
ACCELERATORS = [
    ("A100-80GB", "nvidia", 80),
    ("H100-80GB", "nvidia", 200),
    ("H200-141GB", "nvidia", 300),
    ("MI300X-192GB", "amd", 180),
    ("TPU-v5p", "google", 150),
]
LINKS = ["PCIe", "NVLink"]
REGIONS = ["US_WEST", "US_EAST", "EU_WEST"]
NVIDIA = [0, 1, 2]


def split(total, parts, rng):
    """total divided into `parts` positive whole numbers."""
    cuts = sorted(rng.sample(range(1, total), parts - 1)) if parts > 1 else []
    edges = [0] + cuts + [total]
    return [edges[i + 1] - edges[i] for i in range(parts)]


def curve(total, steps, first_price, rising, rng):
    quantities = split(total, steps, rng)
    price = first_price
    out = []
    for q in quantities:
        out.append(f"{q}@{max(1, round(price))}")
        price *= 1 + rng.uniform(0.02, 0.2) * (1 if rising else -1)
    return ",".join(out)


def first_hour(rng):
    return rng.randint(0, 47 - HOURS)


def ask(participant, token, rng):
    name, _, base = rng.choices(ACCELERATORS, weights=[2, 4, 2, 1, 1])[0]
    cap = rng.randint(1, 8)
    steps = rng.randint(1, 3)
    total = max(steps, round(cap * HOURS * rng.uniform(0.5, 1.0)))
    h = first_hour(rng)
    return (f"ask p={participant} t={token} accel={name} link={rng.choice(LINKS)} region={rng.choice(REGIONS)} "
            f"steps={curve(total, steps, base * rng.uniform(0.7, 1.0), True, rng)} hours={h}-{h + HOURS - 1} cap={cap}")


def bid(participant, token, rng):
    if rng.random() < 0.7:  # two neighbouring NVIDIA models, so some awards are lifts
        low = rng.choice([0, 1])
        accel = [ACCELERATORS[low][0], ACCELERATORS[low + 1][0]]
        base = ACCELERATORS[low][2]
        links = LINKS
        regions = [rng.choice(REGIONS)]
    else:  # one model of another vendor, both interconnects, two regions
        name, _, base = rng.choice(ACCELERATORS[3:])
        accel = [name]
        links = LINKS
        regions = rng.sample(REGIONS, 2)
    cap = rng.randint(1, 4)
    steps = rng.randint(1, 3)
    total = max(steps, round(cap * HOURS * rng.uniform(0.5, 1.0)))
    h = first_hour(rng)
    return (f"bid p={participant} t={token} accel={','.join(accel)} link={','.join(links)} region={','.join(regions)} "
            f"steps={curve(total, steps, base * rng.uniform(0.8, 1.3), False, rng)} hours={h}-{h + HOURS - 1} cap={cap}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bids", type=int, default=10000)
    parser.add_argument("--asks", type=int, default=7500)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if args.bids > BUYERS * BIDS_PER_BUYER:
        sys.exit(f"at most {BUYERS * BIDS_PER_BUYER} bids: {BUYERS} buyers with {BIDS_PER_BUYER} open bids each")
    if args.asks > 10000:
        sys.exit("at most 10000 asks")

    rng = random.Random(args.seed)
    print(f"# {args.bids} bids and {args.asks} asks, seed {args.seed}")
    orders = []
    for i in range(args.bids):
        orders.append(bid(BUYER_BASE + i % BUYERS, i // BUYERS + 1, rng))
    for i in range(args.asks):
        orders.append(ask(SELLER_BASE + i % SELLERS, i // SELLERS + 1, rng))
    rng.shuffle(orders)  # arrival order mixes the sides
    print("\n".join(orders))


if __name__ == "__main__":
    main()
