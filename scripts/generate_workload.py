"""Generate a reusable matrix task pool using only the Python standard library."""

import argparse
import json
import random
from pathlib import Path


def bimodal_tasks(count, seed, heavy_ratio, profile):
    """Return the same task multiset in shuffled or heavy-tail order."""
    heavy_count = max(1, round(count * heavy_ratio))
    small_count = count - heavy_count
    random_source = random.Random(seed)

    tasks = []
    for _ in range(small_count):
        tasks.append({
            "matrix_size": random_source.randint(100, 200),
            "seed": random_source.getrandbits(32),
        })
    for _ in range(heavy_count):
        tasks.append({
            "matrix_size": random_source.randint(700, 900),
            "seed": random_source.getrandbits(32),
        })

    if profile == "bimodal-shuffled":
        # A separate RNG keeps the task multiset identical to heavy-tail;
        # only its order changes between the two profiles.
        random.Random(seed ^ 0xB1A0DA1).shuffle(tasks)
    return tasks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile",
        choices=("uniform", "bimodal-shuffled", "heavy-tail"),
        default="uniform",
    )
    parser.add_argument(
        "--count",
        type=int,
        help="task count (default: 1000 for uniform, 120 for bimodal profiles)",
    )
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--min-size", type=int, default=100)
    parser.add_argument("--max-size", type=int, default=500)
    parser.add_argument("--heavy-ratio", type=float, default=0.2)
    parser.add_argument("--output", type=Path, default=Path("workloads/default.jsonl"))
    args = parser.parse_args()
    count = args.count if args.count is not None else (
        1000 if args.profile == "uniform" else 120
    )
    if count < 1 or not 1 <= args.min_size <= args.max_size <= 1024:
        parser.error("count must be positive; sizes must satisfy 1 <= min <= max <= 1024")
    if not 0.0 < args.heavy_ratio < 1.0:
        parser.error("heavy-ratio must be between 0 and 1")
    if args.profile != "uniform" and count < 2:
        parser.error("bimodal profiles require at least two tasks")

    if args.profile == "uniform":
        random_source = random.Random(args.seed)
        tasks = [
            {
                "matrix_size": random_source.randint(args.min_size, args.max_size),
                "seed": random_source.getrandbits(32),
            }
            for _ in range(count)
        ]
    else:
        tasks = bimodal_tasks(
            count, args.seed, args.heavy_ratio, args.profile
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with args.output.open("x", encoding="utf-8", newline="\n") as output:
            for task_id, descriptor in enumerate(tasks, start=1):
                task = {"task_id": task_id, **descriptor}
                output.write(json.dumps(task, separators=(",", ":")) + "\n")
    except FileExistsError:
        parser.error(f"{args.output} already exists; choose another output to preserve the pool")
    print(
        f"Saved {count} {args.profile} tasks to {args.output} "
        f"(generator seed={args.seed})"
    )


if __name__ == "__main__":
    main()
