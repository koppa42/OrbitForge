from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import unittest
import orbitforge as of

BASE = 8_000_000_000_000_000_000
REFERENCE = Path(__file__).resolve().parents[3] / "tests/reference/propagators/orbit_interpolation.txt"


def options(method=of.OrbitInterpolationMethod.HERMITE, order=7):
    result = of.OrbitInterpolationOptions()
    result.method, result.order = method, order
    return result


def node(t, shift=0):
    return of.CartesianState(of.Epoch(BASE + t * 10**9), of.OriginId.EARTH, of.FrameId.GCRF,
                             (1e6 + t**3 + shift, t*t, 3*t), (3*t*t, 2*t, 3))


class OrbitInterpolationTests(unittest.TestCase):
    def test_installed_api_matches_all_independent_reference_orders(self):
        count = 0
        for line in REFERENCE.read_text(encoding="utf-8").splitlines():
            if line.startswith("#"):
                continue
            fields = line.split()
            if fields[0] == "C":
                _, kind, method, order, size = fields
                config = options(of.OrbitInterpolationMethod.HERMITE if int(method) else
                                 of.OrbitInterpolationMethod.LAGRANGE, int(order))
                nodes, session = [], None
            else:
                epoch = of.Epoch(BASE + int(fields[1]))
                values = list(map(float, fields[2:]))
                if fields[0] == "N":
                    nodes.append(of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.GCRF,
                                                   values[:3], values[3:]))
                else:
                    if session is None:
                        self.assertEqual(len(nodes), int(size))
                        session = of.make_tabulated_orbit_session([nodes], config)
                    state = session.sample([epoch]).states[0]
                    with self.subTest(method=method, order=order, time=fields[1]):
                        for actual, expected in zip(state.position_m, values[:3]):
                            self.assertAlmostEqual(actual, expected, delta=2e-5)
                        for actual, expected in zip(state.velocity_mps, values[3:]):
                            self.assertAlmostEqual(actual, expected, delta=2e-7)
                    count += 1
        self.assertEqual(count, 826)

    def test_arc_ownership_boundaries_provenance_and_concurrent_queries(self):
        before, after, later = [node(0), node(2)], [node(2, 1000), node(4, 1000)], [node(6), node(8)]
        config = options(order=3)
        session = of.make_tabulated_orbit_session([before, after, later], config, "sampled", 7)
        before.clear()
        config.order = 29
        self.assertEqual(session.provenance.object_id, "sampled")
        self.assertEqual(session.provenance.input_revision, 7)
        self.assertIn("order=3", session.provenance.algorithm)
        def query(ns):
            return session.sample([of.Epoch(BASE + ns)]).states[0]
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(query, [2*10**9, 1, 2*10**9-1, 2*10**9+1]))
        self.assertEqual(results[0].position_m, node(2, 1000).position_m)
        self.assertAlmostEqual(results[2].position_m[0], 1e6+8, delta=2e-8)
        self.assertAlmostEqual(results[3].position_m[0], 1e6+1008, delta=2e-8)
        for ns in (-1, 4*10**9+1, 5*10**9, 8*10**9+1):
            with self.assertRaises(of.PropagationError):
                query(ns)
        self.assertEqual(query(2*10**9).position_m, results[0].position_m)
        scenario_session = of.make_tabulated_orbit_session([[node(0), node(2)], after, later], options(order=3))
        scenario = of.Scenario("sampled", of.Epoch(BASE), of.Epoch(BASE+8*10**9)).with_object(scenario_session)
        self.assertEqual(scenario.sample([of.Epoch(BASE+2*10**9)])[0].states[0].position_m, results[0].position_m)

    def test_invalid_inputs_and_no_silent_order_reduction(self):
        for config in (options(order=2), options(order=30), options(order=9)):
            with self.assertRaises(ValueError):
                of.make_tabulated_orbit_session([[node(0), node(1), node(2), node(3)]], config)
        for arcs in ([], [[]], [[node(0), node(0)]], [[node(0), node(3)], [node(2), node(4)]]):
            with self.assertRaises(ValueError):
                of.make_tabulated_orbit_session(arcs, options(order=3))
        with self.assertRaises(ValueError):
            of.make_tabulated_orbit_session([[node(0), node(1)]], options(order=3), "")

    def test_zero_order_keeps_left_sample_and_exact_nodes(self):
        session = of.make_tabulated_orbit_session([[node(0), node(2), node(4)]],
                                                  options(of.OrbitInterpolationMethod.LAGRANGE, 0))
        result = session.sample([of.Epoch(BASE+2*10**9-1), of.Epoch(BASE+2*10**9)])
        self.assertEqual(result.states[0].position_m, node(0).position_m)
        self.assertEqual(result.states[1].position_m, node(2).position_m)
