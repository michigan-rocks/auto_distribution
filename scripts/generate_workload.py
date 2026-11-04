"""Generate a reusable matrix task pool using only the Python standard library."""

import argparse
import json
import random
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--min-size", type=int, default=100)
    parser.add_argument("--max-size", type=int, default=500)
    parser.add_argument("--output", type=Path, default=Path("workloads/default.jsonl"))
    args = parser.parse_args()
    if args.count < 1 or not 1 <= args.min_size <= args.max_size <= 1024:
        parser.error("count must be positive; sizes must satisfy 1 <= min <= max <= 1024")

    random_source = random.Random(args.seed)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with args.output.open("x", encoding="utf-8", newline="\n") as output:
            for task_id in range(1, args.count + 1):
                task = {
                    "task_id": task_id,
                    "matrix_size": random_source.randint(args.min_size, args.max_size),
                    "seed": random_source.getrandbits(32),
                }
                output.write(json.dumps(task, separators=(",", ":")) + "\n")
    except FileExistsError:
        parser.error(f"{args.output} already exists; choose another output to preserve the pool")
    print(f"Saved {args.count} tasks to {args.output} (generator seed={args.seed})")


if __name__ == "__main__":
    main()
