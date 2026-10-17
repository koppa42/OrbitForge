"""Independent SRP disk-union reference using vertical slices and numerical quadrature.

This integrates covered intervals inside the unit solar disk. It does not use the
C++ boundary-arc integration or OrbitForge's bindings. Only Python's standard library
is required. Coordinates/radii are in units of the apparent solar angular radius.
"""
import argparse
import math
from pathlib import Path
import random


def integrate(function, a, b, tolerance=2e-12, depth=24):
    middle = (a + b) / 2
    fa, fm, fb = function(a), function(middle), function(b)
    whole = (b - a) * (fa + 4 * fm + fb) / 6

    def split(a, b, fa, fm, fb, whole, tolerance, depth):
        middle = (a + b) / 2
        fl, fr = function((a + middle) / 2), function((middle + b) / 2)
        left = (middle - a) * (fa + 4 * fl + fm) / 6
        right = (b - middle) * (fm + 4 * fr + fb) / 6
        error = left + right - whole
        if depth == 0 or abs(error) <= 15 * tolerance:
            return left + right + error / 15
        return (split(a, middle, fa, fl, fm, left, tolerance / 2, depth - 1) +
                split(middle, b, fm, fr, fb, right, tolerance / 2, depth - 1))

    return split(a, b, fa, fm, fb, whole, tolerance, depth)


def lighting_ratio(disks):
    disks = [disk for disk in disks if disk[2] > 0]
    knots = [-1.0, 1.0]
    circles = [(0, 0, 1), *disks]
    for x, _, radius in disks:
        knots.extend(value for value in (x - radius, x + radius) if -1 < value < 1)
    for i, (x, y, radius) in enumerate(circles):
        for ox, oy, other in circles[i + 1:]:
            dx, dy = ox - x, oy - y
            distance = math.hypot(dx, dy)
            if distance == 0 or distance > radius + other or distance < abs(radius - other):
                continue
            along = (radius * radius - other * other + distance * distance) / (2 * distance)
            perpendicular = math.sqrt(max(0, radius * radius - along * along))
            centre = x + along * dx / distance
            knots.extend(value for value in (centre - perpendicular * dy / distance,
                                             centre + perpendicular * dy / distance)
                         if -1 < value < 1)
    knots.sort()
    unique = [knots[0]]
    for value in knots[1:]:
        if value - unique[-1] > 1e-14:
            unique.append(value)

    def covered_slice(x):
        solar = math.sqrt(max(0, 1 - x * x))
        intervals = []
        for cx, cy, radius in disks:
            remainder = radius * radius - (x - cx) ** 2
            if remainder <= 0:
                continue
            height = math.sqrt(remainder)
            lower, upper = max(-solar, cy - height), min(solar, cy + height)
            if upper > lower:
                intervals.append((lower, upper))
        total, end = 0.0, -solar
        for lower, upper in sorted(intervals):
            if upper > end:
                total += upper - max(lower, end)
                end = upper
        return total

    area = 0.0
    for a, b in zip(unique, unique[1:]):
        middle, half = (a + b) / 2, (b - a) / 2
        # A sine substitution smooths square-root behaviour at interval endpoints.
        area += integrate(lambda t: covered_slice(middle + half * math.sin(t)) * half * math.cos(t),
                          -math.pi / 2, math.pi / 2)
    return min(1, max(0, 1 - area / math.pi))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cases = [(0, 0, 2, 0, 0, .25), (0, 0, .5, 0, 0, .5), (0, 0, .8, 0, 0, .2),
             (.5, 0, .25, -.5, 0, .25), (1, 0, 1, 0, 0, 0), (-.5, 0, .5, 0, 0, 0),
             (.5, 0, .5, 0, 0, 0), (.25, 0, .25, -.25, 0, .25),
             (.4, .2, .75, -.3, .1, .75), (0, 0, 0, 0, 0, .5), (0, 0, 0, 0, 0, 0),
             (2, 0, 1, -2, 0, 1), (0, 0, 2, 0, 0, 2), (.1, -.1, .5, .1 + 1e-15, -.1, .5)]
    random_source = random.Random(0x535250)
    for _ in range(128):
        cases.append((random_source.uniform(-2, 2), random_source.uniform(-2, 2),
                      random_source.uniform(.02, 2), random_source.uniform(-1.5, 1.5),
                      random_source.uniform(-1.5, 1.5), random_source.uniform(.02, 1.25)))
    for radius in (10, 50, 150, 300):
        for offset in (-.9, -.3, 0, .3, .9):
            for moon_radius in (.25, .75):
                cases.append((radius + offset, .5, radius, -.2, .2, moon_radius))
    rows = ["# Independent vertical-slice quadrature; unit apparent solar disk; deterministic seed 0x535250",
            "# earth_x earth_y earth_radius moon_x moon_y moon_radius visible_fraction"]
    for values in cases:
        ratio = lighting_ratio([values[:3], values[3:]])
        rows.append(" ".join(format(value, ".17g") for value in (*values, ratio)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {len(cases)} independent disk-union references")


if __name__ == "__main__":
    main()
