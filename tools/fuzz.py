"""Differential fuzzer: the C engine must produce exactly the same output as
the Python reference model on random order flow.

Some runs also simulate a crash: run the engine with a journal on the first
part of the flow, chop the journal file at a random byte (a torn write), then
restart on the rest. After recovery the engine must behave exactly as if only
the fully written records had ever been received.

    python3 tools/fuzz.py --binary build/matchbook-san --runs 300
"""
from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import reference  # noqa: E402

RECORD_SIZE = 32  # must match JR_RECORD_SIZE in include/journal.h


def random_flow(rng: random.Random, n: int, max_price: int) -> list[str]:
    """Small price range and id space so orders collide, cross, and get reused a lot."""
    cmds = []
    next_id = 1
    for _ in range(n):
        r = rng.random()
        side = rng.choice("BS")
        if r < 0.55:
            # Mostly valid, occasionally an edge or invalid value.
            price = rng.choice([rng.randint(1, max_price)] * 18 + [0, max_price + 1])
            qty = rng.choice([rng.randint(1, 20)] * 19 + [0])
            oid = next_id if rng.random() < 0.9 else rng.randint(0, next_id)
            next_id += 1
            cmds.append(f"L {oid} {side} {price} {qty}")
        elif r < 0.80:
            cmds.append(f"C {rng.randint(0, next_id)}")
        elif r < 0.93:
            cmds.append(f"M {next_id} {side} {rng.randint(0, 30)}")
            next_id += 1
        else:
            cmds.append("P")
    return cmds


def run_engine(binary: str, cmds: list[str], max_price: int, max_orders: int, journal: str | None = None) -> list[str]:
    args = [binary, "-p", str(max_price), "-n", str(max_orders)]
    if journal:
        args += ["-j", journal, "--fsync-every", "1"]
    proc = subprocess.run(args, input="\n".join(cmds) + "\n", capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"engine exited {proc.returncode}: {proc.stderr}")
    return proc.stdout.splitlines()


def flatten(per_command: list[list[str]]) -> list[str]:
    return [line for out in per_command for line in out]


def first_diff(a: list[str], b: list[str]) -> str:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"line {i}: engine={x!r} reference={y!r}"
    return f"length differs: engine={len(a)} reference={len(b)}"


def plain_run(binary: str, seed: int) -> str | None:
    rng = random.Random(seed)
    max_price = rng.choice([5, 20, 100])
    max_orders = rng.choice([3, 10, 50, 1000])
    cmds = random_flow(rng, rng.randint(50, 1500), max_price)
    got = run_engine(binary, cmds, max_price, max_orders)
    want = flatten(reference.run(cmds, max_price, max_orders))
    if got != want:
        Path(f"fuzz-failure-{seed}.txt").write_text("\n".join(cmds) + "\n")
        return f"seed {seed} (-p {max_price} -n {max_orders}): {first_diff(got, want)}"
    return None


def crash_run(binary: str, seed: int) -> str | None:
    rng = random.Random(seed)
    max_price = rng.choice([5, 20, 100])
    max_orders = rng.choice([10, 1000])
    cmds = [c for c in random_flow(rng, rng.randint(100, 800), max_price) if c != "P"]
    split = rng.randint(1, len(cmds) - 1)
    first, second = cmds[:split], cmds[split:]

    with tempfile.TemporaryDirectory() as tmp:
        journal = os.path.join(tmp, "journal.bin")
        run_engine(binary, first, max_price, max_orders, journal)
        size = os.path.getsize(journal)
        if size != split * RECORD_SIZE:
            return f"seed {seed}: journal has {size} bytes, expected {split * RECORD_SIZE}"

        cut = rng.randint(0, size)  # torn write anywhere, including mid-record
        with open(journal, "r+b") as f:
            f.truncate(cut)
        survived = cut // RECORD_SIZE

        got = run_engine(binary, second, max_price, max_orders, journal)

    per_command = reference.run(first[:survived] + second, max_price, max_orders)
    want = flatten(per_command[survived:])
    if got != want:
        return f"seed {seed} crash run (split {split}, cut {cut}): {first_diff(got, want)}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default="build/matchbook")
    parser.add_argument("--runs", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1, help="first seed")
    args = parser.parse_args()

    failures = []
    for i in range(args.runs):
        seed = args.seed + i
        check = crash_run if i % 4 == 3 else plain_run
        err = check(args.binary, seed)
        if err:
            failures.append(err)
            print("MISMATCH", err, file=sys.stderr)
    crash_runs = args.runs // 4
    print(f"{args.runs} runs ({args.runs - crash_runs} differential, {crash_runs} crash-recovery), {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
