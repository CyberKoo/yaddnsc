#!/usr/bin/env python3
"""Two-track coverage gate for yaddnsc.

GCC splits every coroutine into ramp + .actor + .destroy clones and gcov's
line table collapses the clones, so the line report for a TU that defines
coroutines only covers the ramp and the non-coroutine helpers — the body
(cancellation, timeout budgets, redirect loops) vanishes, skewing line
coverage optimistic. Branch records survive the collapse, so coroutine TUs
are gated on branch coverage instead of line coverage.

Tracks:
  - TU containing co_await/co_return/co_yield: branch coverage >= --branch-min
  - every other TU:                            line coverage >= --line-min

The input tracefile must contain branch records (fastcov -b); a line-only
file is rejected so the gate cannot silently pass without branch data.

The tracefile must also carry each source file exactly once. This gate keys
files by path, so a repeated SF: overwrites the earlier record and only the
last is scored, while lcov and Codecov aggregate every record. A tracefile
with duplicates therefore scores as healthy here and publishes as something
else entirely, so duplicates are rejected rather than scored.

Branch records must exclude the compiler-generated exceptional edges: every
new/destructor/throw emits implicit EH branches that no test can reach, so
including them makes C++ branch coverage non-comparable. Paths a test genuinely
cannot drive (a close() that only fails on a full disk, a pthread primitive
failing under resource exhaustion) are marked LCOV_EXCL_BR_LINE at the branch
site rather than chased with synthetic tests.

Thresholds are ratchets: raise them as coverage improves, never lower them.
"""

import argparse
import re
import sys

CO_RE = re.compile(r"\bco_await\b|\bco_return\b|\bco_yield\b")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def parse_tracefile(path):
    files = {}
    duplicates = []
    current = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("SF:"):
                current = line[3:].strip()
                if current in files:
                    duplicates.append(current)
                files[current] = {"lf": 0, "lh": 0, "br": 0, "brh": 0}
            elif current is None:
                continue
            elif line.startswith("LF:"):
                files[current]["lf"] = int(line[3:])
            elif line.startswith("LH:"):
                files[current]["lh"] = int(line[3:])
            elif line.startswith("BRDA:"):
                taken = line[5:].strip().split(",")[3]
                files[current]["br"] += 1
                if taken != "-" and int(taken) > 0:
                    files[current]["brh"] += 1
    return files, duplicates


def is_coroutine_tu(path):
    """A TU is coroutine-dense if its source defines at least one coroutine."""
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            source = handle.read()
    except OSError:
        return False
    return bool(CO_RE.search(COMMENT_RE.sub("", source)))


def percent(hit, found):
    return 100.0 * hit / found if found else 100.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tracefile", help="lcov tracefile with branch records (fastcov -B)")
    parser.add_argument("--line-min", type=float, default=80.0,
                        help="line coverage floor for non-coroutine TUs (default: 80)")
    parser.add_argument("--branch-min", type=float, default=48.0,
                        help="branch coverage floor for coroutine TUs (default: 48)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="list every TU with its track assignment")
    args = parser.parse_args()

    try:
        files, duplicates = parse_tracefile(args.tracefile)
    except OSError as exc:
        print(f"coverage-gate: cannot read {args.tracefile}: {exc}", file=sys.stderr)
        return 2
    if not files:
        print(f"coverage-gate: {args.tracefile} contains no source files", file=sys.stderr)
        return 2
    if not any(v["br"] for v in files.values()):
        print(f"coverage-gate: {args.tracefile} has no branch records — regenerate with fastcov -b",
              file=sys.stderr)
        return 2
    if duplicates:
        # This gate keys files by path, so a repeated SF: silently overwrites
        # the earlier record and only the last one is ever scored. lcov and
        # Codecov instead aggregate every record, so a tracefile carrying
        # duplicates passes here while the published report disagrees. Scoring
        # it would be meaningless — reject the file instead.
        paths = sorted(set(duplicates))
        print(f"coverage-gate: {len(duplicates)} repeated SF record(s) across "
              f"{len(paths)} file(s) in {args.tracefile}", file=sys.stderr)
        for path in paths[:10]:
            print(f"  {path}", file=sys.stderr)
        if len(paths) > 10:
            print(f"  ... and {len(paths) - 10} more", file=sys.stderr)
        print("coverage-gate: FAIL — this gate scores only the last record per path, "
              "so duplicates read as healthy here while lcov and Codecov aggregate "
              "every record and publish a different number. Regenerate the tracefile "
              "so each source file appears exactly once.", file=sys.stderr)
        return 2

    coroutine_tus = [f for f in files if is_coroutine_tu(f)]
    plain_tus = [f for f in files if f not in set(coroutine_tus)]

    branch_hit = sum(files[f]["brh"] for f in coroutine_tus)
    branch_found = sum(files[f]["br"] for f in coroutine_tus)
    line_hit = sum(files[f]["lh"] for f in plain_tus)
    line_found = sum(files[f]["lf"] for f in plain_tus)
    branch_pct = percent(branch_hit, branch_found)
    line_pct = percent(line_hit, line_found)

    print(f"coroutine TUs ({len(coroutine_tus):3d} files): branches {branch_hit}/{branch_found}"
          f" = {branch_pct:5.1f}%  (gate >= {args.branch_min:.1f}%)")
    print(f"other TUs     ({len(plain_tus):3d} files): lines    {line_hit}/{line_found}"
          f" = {line_pct:5.1f}%  (gate >= {args.line_min:.1f}%)")

    weakest = sorted(
        ((percent(v["brh"], v["br"]), v["br"], f) for f, v in files.items() if f in coroutine_tus and v["br"]),
    )[:10]
    if weakest:
        print("\nweakest coroutine TUs (branch):")
        for pct, found, path in weakest:
            print(f"  {pct:5.1f}%  ({found:4d} branches)  {path}")

    if args.verbose:
        print("\ntrack assignment:")
        for f in sorted(files):
            track = "coroutine" if f in set(coroutine_tus) else "line"
            print(f"  [{track:9s}] {f}")

    failed = []
    if branch_pct < args.branch_min:
        failed.append(f"coroutine-TU branch coverage {branch_pct:.1f}% < {args.branch_min:.1f}%")
    if line_pct < args.line_min:
        failed.append(f"non-coroutine line coverage {line_pct:.1f}% < {args.line_min:.1f}%")
    for reason in failed:
        print(f"\ncoverage-gate: FAIL — {reason}", file=sys.stderr)
    if failed:
        return 1
    print("\ncoverage-gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
