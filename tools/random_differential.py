#!/usr/bin/env python3
"""Generate deterministic valid Arturo programs for four-engine parity tests."""

from __future__ import annotations

import argparse
import random
from pathlib import Path


OPS = ("+", "-", "*")


def integer(value: int) -> str:
    return f"(neg {abs(value)})" if value < 0 else str(value)


def scalar(rng: random.Random, depth: int = 0) -> str:
    if depth >= 3 or rng.random() < 0.38:
        return integer(rng.randint(-30, 30))
    left = scalar(rng, depth + 1)
    right = scalar(rng, depth + 1)
    return f"({left} {rng.choice(OPS)} {right})"


def program(seed: int) -> str:
    rng = random.Random(seed)
    values = [rng.randint(0, 25) for _ in range(rng.randint(3, 8))]
    block = " ".join(integer(value) for value in values)
    needle = rng.choice(values)
    repeat_count = rng.randint(0, 4)
    slice_start = rng.randint(0, len(values) - 1)
    slice_end = rng.randint(slice_start, len(values) - 1)
    text = "".join(rng.choice("abcxyz") for _ in range(rng.randint(3, 10)))
    old = rng.choice("abc")
    new = rng.choice(("", "q", "long"))
    return "\n".join(
        (
            f"print {scalar(rng)}",
            f"vals: [{block}]",
            "print size vals",
            "print reverse vals",
            f"print contains? vals {integer(needle)}",
            f"print slice vals {slice_start} {slice_end}",
            f'print repeat "{text}" {repeat_count}',
            f's: "{text}"',
            f'replace \'s "{old}" "{new}"',
            "print s",
            "print sort vals",
            "",
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(program(args.seed), encoding="utf-8")


if __name__ == "__main__":
    main()
