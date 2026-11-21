# SPDX-License-Identifier: Apache-2.0

import argparse
import itertools
from pathlib import Path
import random
from statistics import median
import subprocess


PARAMETERS = {
    "p324_3": (3, 324, 6),
    "p500_27": (27, 500, 8),
    "p664_17": (17, 664, 11),
}


def run(binary, cpu, *args, data=None):
    command = ["taskset", "-c", str(cpu), str(binary.resolve()), *map(str, args)]
    result = subprocess.run(command, input=data, capture_output=True)
    if result.returncode:
        detail = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(f"{binary} {args[0]} exited with {result.returncode}: {detail}")
    return result.stdout


def verify_field(builds, cpu, parameters):
    factor, exponent, limbs = parameters
    p = (factor << exponent) - 1
    bound = 1 << p.bit_length()
    r_inverse = pow(2, -64 * limbs, p)
    coefficient_bytes = 8 * limbs
    element_bytes = 2 * coefficient_bytes

    def field_product(a, b):
        re = (a[0] * b[0] - a[1] * b[1]) * r_inverse % p
        im = (a[0] * b[1] + a[1] * b[0]) * r_inverse % p
        return re, im

    rng = random.Random(20261120)
    edges = [
        0, 1, 2, (1 << 60) - 1, 1 << 60, (1 << 64) - 1, 1 << 64,
        (1 << (exponent - 60)) - 1, (1 << (64 * (limbs - 1))) - 1, 1 << exponent,
        p - 2, p - 1, p, p + 1, bound - 2, bound - 1,
    ]
    vectors = [((x, y), (y, x)) for x, y in itertools.product(edges, repeat=2)]
    vectors += [
        ((rng.randrange(bound), rng.randrange(bound)), (rng.randrange(bound), rng.randrange(bound)))
        for _ in range(4096)
    ]
    fixture = b"".join(word.to_bytes(coefficient_bytes, "little") for a, b in vectors for word in (*a, *b))
    for side, limit in zip(builds, (bound, p)):
        output = run(builds[side], cpu, "check", data=fixture)
        if len(output) != 5 * len(vectors) * element_bytes:
            raise ValueError(f"Wrong number of field results in {side}")
        for i, (a, b) in enumerate(vectors):
            # The five results cover distinct buffers and four supported aliases.
            expected = (field_product(a, b),) * 3 + (field_product(a, a),) * 2
            for alias, pair in enumerate(expected):
                offset = (5 * i + alias) * element_bytes
                values = tuple(
                    int.from_bytes(output[k:k + coefficient_bytes], "little")
                    for k in (offset, offset + coefficient_bytes)
                )
                if max(values) >= limit or tuple(x % p for x in values) != pair:
                    raise ValueError(f"{side} field mismatch at vector {i}, alias pattern {alias}")
    print(f"Both builds passed {5 * len(vectors):,} field checks including edges and aliases.", flush=True)


def measure_field(builds, cpu, orders):
    calls, rounds = 65_536, 13
    samples = {side: [] for side in builds}
    expected_digest = None
    for i in range(rounds):
        for side in orders[i % len(orders)]:
            cycles, digest = run(builds[side], cpu, "field", calls).decode().strip().split(",")
            if expected_digest is None:
                expected_digest = digest
            if digest != expected_digest:
                raise ValueError("Dependent field chains produced different results")
            samples[side].append(int(cycles) / calls)

    before, after = (median(samples[side]) for side in builds)
    print(f"fp2_mul median ({rounds} rounds of {calls:,} calls)", flush=True)
    print(f"  {before:,.3f} → {after:,.3f} cycles/call, saving {100 * (before - after) / before:.2f}%", flush=True)


def measure_api(builds, cpu, orders, scratch, trials):
    totals = {side: [0] * 6 for side in builds}
    paths = {side: scratch / f"{side}.signed" for side in builds}
    for batch, start in enumerate(range(0, trials, 20)):
        count = min(20, trials - start)
        for side in orders[batch % len(orders)]:
            rows = run(builds[side], cpu, "api", start, count, paths[side]).decode().splitlines()
            expected_trials = set(range(start, start + count))
            for row in rows:
                trial, *values = map(int, row.split(","))
                if trial not in expected_trials or len(values) != 6 or min(values) <= 0:
                    raise ValueError(f"Invalid API measurement in {side} at trial {trial}")
                expected_trials.remove(trial)
                for i, value in enumerate(values):
                    totals[side][i] += value
            if expected_trials:
                raise ValueError(f"Missing API measurements in {side}")

        before = paths["Before"].read_bytes()
        after = paths["After"].read_bytes()
        if before != after:
            raise ValueError(f"Paired public keys or signatures differ in batch starting at {start}")
        for side, fixture in (("Before", after), ("After", before)):
            checked = int(run(builds[side], cpu, "cross-verify", start, data=fixture))
            if checked != count:
                raise ValueError(f"{side} did not verify every signature from the other build")
        print(f"Paired and cross-verified {start + count}/{trials} API trials.", flush=True)

    print(f"API averages over {trials} paired trials")
    print(f"{'Operation':<10} {'Before Mcycles':>15} {'After Mcycles':>15} {'Saved':>8}"
          f" {'Before ms':>12} {'After ms':>12} {'Saved':>8}")
    before, after = (totals[side] for side in builds)
    for i, operation in enumerate(("Keygen", "Sign", "Verify")):
        cycles_saved = 100 * (before[i] - after[i]) / before[i]
        time_saved = 100 * (before[i + 3] - after[i + 3]) / before[i + 3]
        scale = trials * 1_000_000
        print(f"{operation:<10} {before[i] / scale:>15,.3f} {after[i] / scale:>15,.3f} {cycles_saved:>7.2f}%"
              f" {before[i + 3] / scale:>12,.3f} {after[i + 3] / scale:>12,.3f} {time_saved:>7.2f}%")


def main():
    parser = argparse.ArgumentParser(description="Compare two builds of one SQIsign parameter set.")
    parser.add_argument("variant", choices=PARAMETERS)
    parser.add_argument("trials", type=int)
    parser.add_argument("cpu", type=int)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    if args.trials < 1 or args.cpu < 0:
        parser.error("trials must be positive and cpu must be non-negative")

    builds = {"Before": args.before, "After": args.after}
    orders = tuple(itertools.permutations(builds))
    verify_field(builds, args.cpu, PARAMETERS[args.variant])
    measure_field(builds, args.cpu, orders)
    measure_api(builds, args.cpu, orders, args.scratch, args.trials)


if __name__ == "__main__":
    main()
