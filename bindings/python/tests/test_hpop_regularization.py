"""Public regularization settings, scientific results and transactional boundaries."""
from dataclasses import replace
from pathlib import Path
import math
import tempfile
import unittest

import orbitforge as of
from orbitforge import units as u


class HpopRegularizationTests(unittest.TestCase):
    def make(self, **options):
        project = of.Project("Regularized", of.Epoch(8*10**18), of.Epoch(8*10**18 + 600*10**9 + 17))
        initial = of.CartesianState(project.first, of.OriginId.EARTH, of.FrameId.GCRF,
                                    [7e6, 0, 0], [0, math.sqrt(3.986004415e14*1.7/7e6), 0])
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0, time_regularization=True,
                              regularization_exponent=1.5, steps_per_orbit=8192, **options)
        satellite = project.objects.satellite("HPOP", orbit=orbit)
        return project, satellite, initial

    def test_all_methods_roundtrip_identity_and_independent_cspice(self):
        for method in ("rk4", "rkf78", "rkv89", "bulirsch_stoer", "gauss_jackson"):
            for control in ("fixed", "relative_error"):
                if control == "relative_error" and method in ("rk4", "gauss_jackson"):
                    continue
                with self.subTest(method=method, control=control):
                    project, satellite, initial = self.make(integrator=method, step_control=control)
                    analysis = project.prepare()
                    when = of.Epoch(project.first.tai_nanoseconds + 123_123_456_789)
                    before = analysis.states(satellite, times=[when, project.last])
                    native = satellite.orbit.hpop._native()
                    self.assertTrue(native.time_regularization)
                    self.assertEqual(native.regularization_exponent, 1.5)
                    self.assertEqual(native.steps_per_orbit, 8192)
                    self.assertIn("time=sundman-initial-ellipse-bounded-v1", before.provenance.algorithm)
                    if of.two_body_available():
                        truth = of.make_two_body_session(initial, 3.986004415e14, "cspice").sample([when, project.last])
                        for actual, expected in zip(before, truth.states):
                            self.assertLess(math.dist(actual.position_m, expected.position_m), .005)
                            self.assertLess(math.dist(actual.velocity_mps, expected.velocity_mps), 5e-6)
                    satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, fixed_step_seconds=47))
                    current = project.prepare().states(satellite, times=[when, project.last])
                    self.assertEqual(current.input_sha256, before.input_sha256)
                    self.assertEqual(current.position.values, before.position.values)
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory)/"regularized.orbitforge"
                        project.save(path)
                        restored = of.Project.open(path)
                        spacecraft = restored.objects[satellite.id]
                        self.assertEqual(spacecraft.orbit.hpop, satellite.orbit.hpop)
                        self.assertEqual(restored.prepare().states(spacecraft, times=[when, project.last]).position.values,
                                         before.position.values)
                    satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, steps_per_orbit=4096))
                    self.assertNotEqual(project.prepare().states(satellite, times=[when]).input_sha256, before.input_sha256)
                    self.assertEqual(analysis.states(satellite, times=[when, project.last]).position.values, before.position.values)

    def test_cancel_retry_and_invalid_edits_preserve_project(self):
        project, satellite, _ = self.make()
        analysis = project.prepare()
        calls = 0
        def cancel():
            nonlocal calls
            calls += 1
            return calls == 20
        with self.assertRaises(of.AnalysisCancelled):
            analysis.report(of.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(60), cancelled=cancel)
        self.assertEqual(calls, 20)
        self.assertEqual(analysis.state(satellite, at=project.last).position_m,
                         project.prepare().state(satellite, at=project.last).position_m)
        revision = project.revision
        for values in ({"time_regularization": 1}, {"steps_per_orbit": True},
                       {"steps_per_orbit": 1.5}, {"steps_per_orbit": 0},
                       {"steps_per_orbit": -1}, {"steps_per_orbit": 2**31},
                       {"regularization_exponent": 0}, {"regularization_exponent": 10.001},
                       {"regularization_exponent": float("nan")}, {"regularization_exponent": float("inf")}):
            with self.subTest(values=values), self.assertRaises((TypeError, ValueError, OverflowError)):
                satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, **values))
        self.assertEqual(project.revision, revision)

    def test_disabled_regularization_drafts_do_not_change_calculation(self):
        project, satellite, _ = self.make()
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, time_regularization=False))
        before = project.prepare().states(satellite, times=[project.last])
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop,
                                  regularization_exponent=2, steps_per_orbit=2048))
        after = project.prepare().states(satellite, times=[project.last])
        self.assertEqual(before.input_sha256, after.input_sha256)
        self.assertEqual(before.position.values, after.position.values)
