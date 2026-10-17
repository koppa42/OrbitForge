"""Universal VOP integration through the independently installed public API."""
from dataclasses import replace
from pathlib import Path
import math
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u

MU = 3.986004415e14
REFERENCE = Path(__file__).resolve().parents[3] / "tests/reference/propagators/universal_kepler.txt"


class HpopVopTests(unittest.TestCase):
    def test_native_single_step_matches_independent_orekit(self):
        cases = [line.split() for line in REFERENCE.read_text(encoding="ascii").splitlines() if not line.startswith("#")]
        for method in (of.HpopIntegrator.RKF78, of.HpopIntegrator.BULIRSCH_STOER):
            for row in cases:
                kind, mu, dt = int(row[0]), float(row[1]), float(row[2])
                y = list(map(float, row[3:]))
                initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF, y[:3], y[3:6])
                options = of.HpopOptions()
                options.gravity_degree = options.gravity_order = 0
                options.gravity_mu = mu
                options.integrator, options.vop = method, True
                options.step_control = of.HpopStepControl.FIXED
                options.fixed_step_seconds = 86400
                when = of.Epoch(round(dt*1e9))
                value = of.OrbitInput()
                value.initial = initial
                value.propagator = of.OrbitPropagator.HPOP
                value.hpop = options
                arcs = of.propagate_hpop_ephemeris(value, when)
                self.assertEqual(len(arcs), 1)
                self.assertEqual(len(arcs[0]), 2)
                actual = arcs[0][-1] if dt > 0 else arcs[0][0]
                with self.subTest(method=method, kind=kind, dt=dt):
                    self.assertTrue(options.uses_vop())
                    self.assertLess(math.dist(actual.position_m, y[6:9]), 5e-4)
                    self.assertLess(math.dist(actual.velocity_mps, y[9:12]), 5e-7)

    def make(self, method="rkf78", regularized=False):
        first = of.Epoch(8*10**18)
        project = of.Project("VOP", first, of.Epoch(first.tai_nanoseconds + 600*10**9 + 17))
        initial = of.CartesianState(first, of.OriginId.EARTH, of.FrameId.GCRF, [7e6, 0, 0], [0, math.sqrt(MU*1.7/7e6), 0])
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0, integrator=method, vop=True,
                              time_regularization=regularized, steps_per_orbit=480,
                              interpolation="vop", interpolation_order=7, report_ephemeris_on_fixed_step=True)
        return project, project.objects.satellite("HPOP", orbit=orbit), initial

    def test_active_and_inactive_drafts_roundtrip_with_immutable_analysis(self):
        for method in ("rk4", "rkf78", "rkv89", "bulirsch_stoer", "gauss_jackson"):
            project, satellite, _ = self.make(method)
            analysis = project.prepare()
            before = analysis.states(satellite, times=[project.last])
            self.assertTrue(satellite.orbit.hpop.vop)
            self.assertEqual(satellite.orbit.hpop._native().uses_vop(), method in ("rkf78", "bulirsch_stoer"))
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)/"vop.orbitforge"
                project.save(path)
                restored = of.Project.open(path)
                self.assertEqual(restored.objects[satellite.id].orbit.hpop, satellite.orbit.hpop)
            satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, vop=False))
            after = project.prepare().states(satellite, times=[project.last])
            if method in ("rkf78", "bulirsch_stoer"):
                self.assertNotEqual(before.input_sha256, after.input_sha256)
                self.assertIn("hpop-vop-universal-osculating", before.provenance.algorithm)
                self.assertIn("hpop-cowell", after.provenance.algorithm)
            else:
                self.assertEqual(before.input_sha256, after.input_sha256)
                self.assertEqual(before.position.values, after.position.values)
            self.assertEqual(before.position.values, analysis.states(satellite, times=[project.last]).position.values)

    def test_regularized_cspice_truth_exact_reporting_and_cancel_retry(self):
        for method in ("rkf78", "bulirsch_stoer"):
            for regularized in (False, True):
                project, satellite, initial = self.make(method, regularized)
                analysis = project.prepare()
                calls = 0
                def cancel():
                    nonlocal calls
                    calls += 1
                    return calls == 20
                with self.assertRaises(of.AnalysisCancelled):
                    analysis.report(of.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(60), cancelled=cancel)
                self.assertEqual(calls, 20)
                when = of.Epoch(project.first.tai_nanoseconds + 123_123_456_789)
                actual = analysis.states(satellite, times=[when, project.last])
                self.assertEqual(actual.position.values, project.prepare().states(satellite, times=[when, project.last]).position.values)
                if of.two_body_available():
                    truth = of.make_two_body_session(initial, MU, "cspice").sample([when, project.last])
                    for state, expected in zip(actual, truth.states):
                        self.assertEqual(state.epoch.tai_nanoseconds, expected.epoch.tai_nanoseconds)
                        self.assertLess(math.dist(state.position_m, expected.position_m), .005)
                        self.assertLess(math.dist(state.velocity_mps, expected.velocity_mps), 5e-6)
                revision = project.revision
                for value in (1, "true", None):
                    with self.assertRaises(TypeError):
                        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, vop=value))
                self.assertEqual(project.revision, revision)
