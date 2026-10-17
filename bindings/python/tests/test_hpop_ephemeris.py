from concurrent.futures import ThreadPoolExecutor
import math
import unittest
import orbitforge as of


def orbit(method=of.HpopIntegrator.RKF78):
    value = of.OrbitInput()
    value.propagator = of.OrbitPropagator.HPOP
    value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                      (7e6, 0, 0), (0, math.sqrt(3.986004415e14/7e6), 0))
    value.object_id = "sampled-hpop"
    config = of.HpopOptions()
    config.gravity_degree = config.gravity_order = 0
    config.integrator = method
    config.fixed_step_seconds = 10
    config.max_step_seconds = 30
    value.hpop = config
    return value


def interpolation():
    value = of.OrbitInterpolationOptions()
    value.method = of.OrbitInterpolationMethod.HERMITE
    value.order = 7
    return value


class HpopEphemerisTests(unittest.TestCase):
    def test_exact_fixed_outputs_and_owned_accepted_nodes(self):
        for method in (of.HpopIntegrator.RK4, of.HpopIntegrator.RKF78, of.HpopIntegrator.RKV89,
                       of.HpopIntegrator.BULIRSCH_STOER, of.HpopIntegrator.GAUSS_JACKSON):
            for sign in (-1, 1):
                with self.subTest(method=method, sign=sign):
                    value = orbit(method)
                    config = value.hpop
                    config.step_control = of.HpopStepControl.FIXED
                    value.hpop = config
                    end = of.Epoch(sign*123_123_456_789)
                    accepted = of.propagate_hpop_ephemeris(value, end)
                    self.assertEqual(len(accepted), 1)
                    self.assertEqual(len(accepted[0]), 14)
                    reported = of.propagate_hpop_ephemeris(value, end, reporting_step_nanoseconds=7_100_000_000)
                    expected = sorted([sign*i*7_100_000_000 for i in range(18)] + [end.tai_nanoseconds])
                    self.assertEqual([s.epoch.tai_nanoseconds for s in reported[0]], expected)
                    value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                                      (8e6, 0, 0), (0, 7000, 0))
                    index = 0 if sign > 0 else -1
                    self.assertEqual(accepted[0][index].position_m, [7e6, 0, 0])

    def test_shared_bounded_session_and_source_identity(self):
        value = orbit()
        config = interpolation()
        session = of.make_hpop_ephemeris_session(value, of.Epoch(600*10**9), config,
                   reporting_step_nanoseconds=30*10**9, input_revision=7)
        self.assertEqual(session.provenance.object_id, value.object_id)
        self.assertEqual(session.provenance.input_revision, 7)
        self.assertIn("report-ns=30000000000", session.provenance.algorithm)
        self.assertIn("method=hermite;interpolation-order=7", session.provenance.algorithm)
        times = [of.Epoch(173_123_456_789), of.Epoch(599_000_000_001)]
        expected = session.sample(times).states
        config.order = 29
        value.hpop = of.HpopOptions()
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: session.sample(times).states, range(3)))
        for result in results:
            self.assertEqual(result[0].position_m, expected[0].position_m)
        for ns in (-1, 600*10**9+1):
            with self.assertRaises(of.PropagationError): session.sample([of.Epoch(ns)])
        if of.two_body_available():
            truth = of.make_two_body_session(orbit().initial, 3.986004415e14, "cspice").sample(times).states
            for actual, reference in zip(expected, truth):
                for a, b in zip(actual.position_m, reference.position_m):
                    self.assertAlmostEqual(a, b, delta=.001)
                for a, b in zip(actual.velocity_mps, reference.velocity_mps):
                    self.assertAlmostEqual(a, b, delta=1e-6)

    def test_cancelled_or_invalid_preparation_does_not_publish(self):
        value = orbit()
        with self.assertRaises(of.AnalysisCancelled):
            of.propagate_hpop_ephemeris(value, of.Epoch(600*10**9), cancelled=lambda: True)
        with self.assertRaises(TypeError):
            of.propagate_hpop_ephemeris(value, of.Epoch(600*10**9), cancelled=lambda: 1)
        with self.assertRaises(of.AnalysisCancelled):
            of.make_hpop_ephemeris_session(value, of.Epoch(600*10**9), interpolation(), cancelled=lambda: True)
        for period in (-1, 86_400*10**9+1):
            with self.assertRaises(ValueError):
                of.propagate_hpop_ephemeris(value, of.Epoch(1), reporting_step_nanoseconds=period)
        with self.assertRaises(ValueError):
            of.make_hpop_ephemeris_session(value, of.Epoch(1), interpolation())
        with self.assertRaises(ValueError):
            of.propagate_hpop_ephemeris(of.OrbitInput(), of.Epoch(1))
        result = of.propagate_hpop_ephemeris(value, of.Epoch(600*10**9))
        def change_input():
            value.propagator = of.OrbitPropagator.TWO_BODY
            value.hpop = of.HpopOptions()
            return False
        frozen = of.propagate_hpop_ephemeris(value, of.Epoch(600*10**9), cancelled=change_input)
        self.assertEqual(frozen[-1][-1].position_m, result[-1][-1].position_m)
