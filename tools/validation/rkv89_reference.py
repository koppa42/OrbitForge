"""Independent orbit reference: SciPy 1.16.3 DOP853, no OrbitForge imports.

python tools/validation/rkv89_reference.py OUTPUT_DIRECTORY
Point mass / closed-form J2 / moving lunar point mass, fixed Cartesian axes.
A 10-second ceiling produces the stored result; a 20-second ceiling certifies
reference convergence. SI state, TAI seconds from epoch zero; no UTC/EOP model.
"""
from pathlib import Path
import json
import sys
import numpy as np
import scipy
from scipy.integrate import solve_ivp

assert scipy.__version__ == '1.16.3'
assert np.__version__ == '2.3.3'
MU, RADIUS = 3.986004415e14, 6378136.3
J2 = np.sqrt(5) * .484165371736e-3
MOON_MU = 4.902800066e12
TIMES = np.array([-86400., -21600., -7200., -1234.567890123, 1e-9,
                  .25, 1234.567890123, 7200., 21600., 86400.])


def moon(t):
    angle = t * 2 * np.pi / (27.321661 * 86400) + .4
    return 384400000 * np.array([np.cos(angle), np.sin(angle) * np.cos(.3),
                                np.sin(angle) * np.sin(.3)])


def derivative(kind, t, state):
    p, v = state[:3], state[3:]
    r = np.linalg.norm(p)
    a = -MU * p / r**3
    if kind:
        z2 = (p[2] / r)**2
        a += 1.5 * J2 * MU * RADIUS**2 / r**5 * p * np.array([5*z2-1, 5*z2-1, 5*z2-3])
    if kind == 2:
        body = moon(t)
        delta = body - p
        a += MOON_MU * (delta / np.linalg.norm(delta)**3 - body / np.linalg.norm(body)**3)
    return np.r_[v, a]


def initial(perigee, eccentricity, inclination):
    speed = np.sqrt(MU * (1 + eccentricity) / perigee)
    return np.array([perigee, 0., 0., 0., speed * np.cos(inclination), speed * np.sin(inclination)])


def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    rows = ['# SciPy 1.16.3 DOP853; NumPy 2.3.3; SI, fixed inertial axes, epoch TAI 0',
            '# kind elapsed_seconds initial_r[3] initial_v[3] propagated_r[3] propagated_v[3]']
    max_r = max_v = 0.
    for kind, perigee, e, inc in [(0, 7e6, 0, 0), (0, 7e6, .7, .9), (0, 7e6, .98, 1.2),
                                 (1, 7e6, .01, 1.7), (1, 26560e3, .12, .95), (2, 7e6, .1, .6)]:
        y0 = initial(perigee, e, inc)
        reference = {}
        for direction in (-1, 1):
            at = TIMES[TIMES * direction > 0][::direction]
            answers = []
            for ceiling in (20., 10.):
                sol = solve_ivp(lambda t, y: derivative(kind, t, y), (0., direction * 86400.), y0,
                                method='DOP853', rtol=3e-14, atol=[1e-9]*3 + [1e-12]*3,
                                max_step=ceiling, t_eval=at)
                assert sol.success, sol.message
                answers.append(sol.y.T)
            errors = answers[0] - answers[1]
            max_r = max(max_r, np.linalg.norm(errors[:, :3], axis=1).max())
            max_v = max(max_v, np.linalg.norm(errors[:, 3:], axis=1).max())
            reference.update(zip(at, answers[1]))
        for t in TIMES:
            rows.append(' '.join([str(kind), format(t, '.17g'),
                                 *[format(v, '.17g') for v in np.r_[y0, reference[t]]]]))
    assert max_r < 5e-5 and max_v < 5e-8, (max_r, max_v)
    (out / 'rkv89.txt').write_text('\n'.join(rows) + '\n', encoding='ascii', newline='\n')
    (out / 'rkv89-reference-convergence.json').write_text(json.dumps({
        'scipy': scipy.__version__, 'numpy': np.__version__, 'rows': len(rows)-2,
        'max_position_change_m': float(max_r), 'max_velocity_change_mps': float(max_v),
        'max_steps_seconds': [20, 10], 'rtol': 3e-14,
    }, indent=2) + '\n', encoding='utf-8')
    print(len(rows)-2, 'reference rows; 20s/10s max change', max_r, 'm,', max_v, 'm/s')


if __name__ == '__main__':
    generate(Path(sys.argv[1]))
