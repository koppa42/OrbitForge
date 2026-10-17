"""Independent exact-axis potential derivatives, 80-digit Decimal arithmetic.

Uses Legendre identities P_n^(m)(+/-1) and harmonicity, without a Cartesian
recurrence or OrbitForge import. Only orders 0, 1 and 2 contribute to the
potential Hessian on the axis; all higher orders vanish exactly. Inputs are
converted from binary64 without decimal rounding. Run with an output path.
"""
import math
import sys
from decimal import Decimal as D, localcontext
from pathlib import Path


def coefficients(profile, n, m):
    a = 1e-8 / ((n + 1.0) * (n + 1))
    c = 0.0 if profile == 2 else a * math.sin((n + 1.0) * (m + 2))
    s = 0.0 if profile == 2 or m == 0 else a * math.cos((n + 1.0) * (m + 2))
    if n == 2 and m == 0:
        c = -4.84165371736e-4
    if profile == 2 and n == 2190:
        c, s = ((1e-8, 0.0), (1e-10, -2e-10), (-2e-10, 3e-10))[m]
    return D.from_float(c), D.from_float(s)


def evaluate(profile, degree, order, altitude):
    mu, radius, z = map(D.from_float, (3.986004415e14, 6378136.3, altitude))
    r = abs(z)
    sign = 1 if z > 0 else -1
    a = [D(0)] * 3
    h = [[D(0) for _ in range(3)] for _ in range(3)]
    for n in [0, *range(2, degree + 1)]:
        common = mu / r * (radius / r)**n
        for m in range(min(n, order, 2) + 1):
            c, s = (D(1), D(0)) if n == 0 else coefficients(profile, n, m)
            if m == 0:
                u = common * D(2*n+1).sqrt() * c * sign**n
                a[2] -= (n+1) * u / z
                zz = (n+1) * (n+2) * u / r**2
                h[2][2] += zz
                h[0][0] -= zz / 2
                h[1][1] -= zz / 2
            elif m == 1:
                norm = (D(2*(2*n+1)) / (n*(n+1))).sqrt()
                factor = common / r * norm * D(n*(n+1))/2 * sign**(n-1)
                a[0] += factor*c
                a[1] += factor*s
                h[0][2] -= (n+2) * factor*c / z
                h[1][2] -= (n+2) * factor*s / z
            else:
                product = (n-1)*n*(n+1)*(n+2)
                norm = (D(2*(2*n+1)) / product).sqrt()
                factor = common / r**2 * norm * D(product)/4 * sign**(n-2)
                h[0][0] += factor*c
                h[1][1] -= factor*c
                h[0][1] += factor*s
    for i in range(3):
        for j in range(i):
            h[i][j] = h[j][i]
    return [*a, *(entry for row in h for entry in row)]


def main():
    lines = ["# Exact-axis Legendre derivatives, Decimal precision 80; binary64 inputs",
             "# profile degree order x y z ax ay az Jxx Jxy Jxz Jyx Jyy Jyz Jzx Jzy Jzz; SI"]
    with localcontext() as context:
        context.prec = 80
        for profile in (0, 2):
            for degree in ((2, 20, 360, 2190) if profile == 0 else (2190,)):
                for order in sorted({0, 1, 2, degree}):
                    for z in (6356752.3, -6356752.3, 7e6, -7e6):
                        values = evaluate(profile, degree, order, z)
                        lines.append(f"{profile} {degree} {order} 0 0 {z:.17g} " +
                                     " ".join(f"{float(v):.17g}" for v in values))
    Path(sys.argv[1]).write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
