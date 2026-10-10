#!/usr/bin/env python3
"""Plant one bug at a time and see whether a test fails.

Each plant in plants.py is a one-line change to a source file: the text it replaces, the text that takes
its place, and how often the old text occurs. For every plant this script makes a private copy of the
tree, applies the change, rebuilds the test binary and runs every test. A plant is

    CAUGHT      if a test fails or crashes, or if the build is refused by a static_assert,
    SURVIVED    if every test passes. Either the tests have a gap or the change cannot matter, so read it,
    BROKEN      if the changed source does not build for another reason, which is a mistake in the plant.

The copies live in a temporary directory, so the repository is never changed. Linux only, because it builds
the tests.

    python3 Tools/Mutation/mutate.py --list                    the plants
    python3 Tools/Mutation/mutate.py --dry                     does every old text occur as often as expected?
    python3 Tools/Mutation/mutate.py [--workers N] [id ...]    run all the plants, or the ones named
"""
import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from plants import PLANTS  # noqa: E402  (id, area, what, file, old text, new text, occurrences)

NOT_COPIED = {"build", "docs", "Tools"}


def sh(cmd, timeout=3600):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


class Worker:
    def __init__(self, root, jobs):
        self.src = root / "src"
        self.build = root / "build"
        self.jobs = jobs

    def setup(self):
        self.src.mkdir(parents=True)
        for item in REPO.iterdir():
            if item.name.startswith(".") or item.name in NOT_COPIED:
                continue
            if item.is_dir():
                shutil.copytree(item, self.src / item.name, ignore=shutil.ignore_patterns("__pycache__"))
            elif item.name == "CMakeLists.txt":
                shutil.copy(item, self.src / item.name)
        deps = REPO / "build" / "_deps"
        offline = ""
        if all((deps / name).exists() for name in ("core-src", "googletest-src", "sqlite3-src")):
            offline = (f" -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_CORE={deps}/core-src"
                       f" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={deps}/googletest-src"
                       f" -DFETCHCONTENT_SOURCE_DIR_SQLITE3={deps}/sqlite3-src")
        configure = sh(f"cmake -S {self.src} -B {self.build} -DCMAKE_BUILD_TYPE=Release{offline}")
        if configure.returncode != 0:
            raise RuntimeError("configure failed:\n" + configure.stdout[-2000:] + configure.stderr[-2000:])
        built = self.rebuild()
        if built[0] != 0:
            raise RuntimeError("the unchanged tree does not build:\n" + "\n".join(built[1]))

    def rebuild(self):
        r = sh(f"cmake --build {self.build} --target ProductTests -j{self.jobs}")
        text = r.stdout + r.stderr
        return r.returncode, [l for l in text.splitlines() if re.search(r"\berror\b", l)]

    def run_tests(self):
        """The names of the tests that failed, how many ran, and a note if the binary crashed or hung."""
        try:
            r = subprocess.run([str(self.build / "ProductTests")], capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            return ["(the tests did not finish in 300 s)"], -1, "timeout"
        failed = sorted(set(m.group(1) for m in re.finditer(r"^\[  FAILED  \] (\S+?)(?:,.*| \(\d+ ms\))?$", r.stdout, re.M)))
        ran = re.search(r"\[==========\] (\d+) tests", r.stdout)
        note = ""
        if r.returncode not in (0, 1) and not ran:
            started = re.findall(r"^\[ RUN      \] (\S+)", r.stdout, re.M)
            failed = sorted(set(failed) | {started[-1] if started else "(before the first test)"})
            note = f"crashed, status {r.returncode}"
        return failed, int(ran.group(1)) if ran else -1, note

    def try_plant(self, plant):
        pid, area, what, rel, old, new, expected = plant
        path = self.src / rel
        original = path.read_bytes()
        entry = {"id": pid, "area": area, "bug": what, "file": rel}
        started = time.time()
        try:
            text = original.decode()
            found = text.count(old)
            if found != expected:
                entry.update(outcome="PATTERN", detail=f"the old text occurs {found} times, expected {expected}")
            else:
                path.write_bytes(text.replace(old, new).encode())
                code, errors = self.rebuild()
                if code != 0 or errors:
                    refused = any("static assertion" in line for line in errors)
                    entry.update(outcome="CAUGHT" if refused else "BROKEN", detail=(errors or [""])[0][:160])
                    if refused:
                        entry["failing"] = ["(refused at compile time)"]
                else:
                    failed, ran, note = self.run_tests()
                    entry.update(outcome="CAUGHT" if failed else "SURVIVED", failing=failed, tests_run=ran)
                    if note:
                        entry["note"] = note
        finally:
            path.write_bytes(original)
        entry["seconds"] = round(time.time() - started)
        return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ids", nargs="*", help="plants to run (default: all)")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry", action="store_true")
    parser.add_argument("--workers", type=int, default=2, help="private copies built and run in parallel")
    parser.add_argument("--jobs", type=int, default=4, help="build jobs per worker")
    parser.add_argument("--out", help="also write one JSON line per plant to this file")
    options = parser.parse_args()

    chosen = [p for p in PLANTS if not options.ids or p[0] in options.ids]
    unknown = set(options.ids) - {p[0] for p in PLANTS}
    if unknown:
        sys.exit("no such plant: " + ", ".join(sorted(unknown)))
    if options.list:
        for pid, area, what, rel, _, _, _ in chosen:
            print(f"{pid:<10} {area:<14} {what}   [{rel}]")
        return
    if options.dry:
        wrong = 0
        for pid, _, _, rel, old, _, expected in chosen:
            found = (REPO / rel).read_bytes().decode().count(old)
            if found != expected:
                print(f"{pid}: the old text occurs {found} times in {rel}, expected {expected}")
                wrong += 1
        print(f"{len(chosen) - wrong} of {len(chosen)} plants match the source")
        sys.exit(1 if wrong else 0)

    workers = max(1, min(options.workers, len(chosen)))
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="batchgpu-mutation-"))
    results, lock = [], threading.Lock()
    out = open(options.out, "w") if options.out else None

    def work(index, plants):
        worker = Worker(scratch / f"w{index}", options.jobs)
        worker.setup()
        for plant in plants:
            entry = worker.try_plant(plant)
            with lock:
                results.append(entry)
                if out:
                    out.write(json.dumps(entry) + "\n")
                    out.flush()
                tests = len(entry.get("failing", []))
                print(f"{entry['id']:<10} {entry['outcome']:<9} {tests:>3} failing  {entry['bug']}", flush=True)

    try:
        # Plants in the same file go to the same worker, so that one header is not rebuilt for everyone.
        ordered = sorted(chosen, key=lambda p: p[3])
        per = (len(ordered) + workers - 1) // workers
        slices = [ordered[w * per:(w + 1) * per] for w in range(workers)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for future in [pool.submit(work, w, s) for w, s in enumerate(slices) if s]:
                future.result()
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
        if out:
            out.close()

    caught = sum(1 for r in results if r["outcome"] == "CAUGHT")
    print(f"\n{caught} of {len(results)} plants caught")
    for r in results:
        if r["outcome"] != "CAUGHT":
            print(f"  {r['outcome']}: {r['id']}  {r['bug']}")
    sys.exit(0 if caught == len(results) else 1)


if __name__ == "__main__":
    main()
