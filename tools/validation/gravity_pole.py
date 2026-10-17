"""Independent exact-axis Legendre identities, evaluated at 70 decimal digits.

No OrbitForge import. Decimal.from_float preserves the production binary64
inputs. For U_n=mu/r*C_n0*sqrt(2n+1)*(R/r)^n*P_n(z/r), use P_n(1)=1
and P_n'(1)=n(n+1)/2. The linear transverse value's neglected O(x^3)
term at x=1e-7 m is far below the test's 1e-20 m/s^2 bound.
"""
from decimal import Decimal as D, localcontext


def main():
    with localcontext() as context:
        context.prec = 70
        mu, radius, z = map(D.from_float, (3.986004415e14, 6378136.3, 6356752.3))
        terms = ((0, D(1)), (2, D.from_float(-4.84165371736e-4)),
                 (2190, D.from_float(1e-8)))
        az = -mu / z**2 * sum(c * D(2*n+1).sqrt() * (radius/z)**n * (n+1)
                               for n, c in terms)
        slope = -mu / z**3 * sum(c * D(2*n+1).sqrt() * (radius/z)**n *
                                  ((n+1) + D(n*(n+1))/2) for n, c in terms)
        print("Polar az (m/s^2):", az)
        print("Transverse ax at x=1e-7 m (m/s^2):", slope * D.from_float(1e-7))


if __name__ == "__main__":
    main()
