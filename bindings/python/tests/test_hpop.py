from dataclasses import FrozenInstanceError, replace
from pathlib import Path
import math
import tempfile
import unittest

import orbitforge as of
from orbitforge import units as u

EOP = Path(__file__).resolve().parents[3] / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"


class HpopTests(unittest.TestCase):
    def make(self, **options):
        project = of.Project("Numerical", of.Epoch(0), of.Epoch(7_200_000_000_000))
        initial = of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, math.sqrt(3.986004415e14 / 7e6), 0))
        satellite = project.objects.satellite("HPOP", orbit=of.orbits.Hpop(initial=initial, **options))
        return project, satellite

    def test_step_control_roundtrip_and_inactive_tolerance_identity(self):
        for method in ("rkf78", "rkv89", "bulirsch_stoer", "rk4", "gauss_jackson"):
            with self.subTest(method=method):
                project, satellite = self.make(gravity_degree=0, integrator=method,
                                               step_control="fixed", fixed_step_seconds=7.1)
                first = project.prepare()
                state = first.state(satellite, at=project.last)
                captured = first.states(satellite, step=u.s(600))
                self.assertEqual(satellite.orbit.hpop._native().step_control, of.HpopStepControl.FIXED)
                self.assertTrue(satellite.orbit.hpop._native().uses_fixed_step())
                satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop,
                    relative_tolerance=1e-14, position_tolerance_m=1e-200,
                    velocity_tolerance_mps=1e-200, min_step_seconds=1e-6, max_step_seconds=1e-6))
                current = project.prepare()
                self.assertEqual(current.states(satellite, step=u.s(600)).input_sha256, captured.input_sha256)
                self.assertEqual(current.state(satellite, at=project.last).position_m, state.position_m)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "fixed.orbitforge"
                    project.save(path, analysis=current)
                    loaded = of.Project.open(path)
                    restored = loaded.objects[satellite.id]
                    self.assertEqual(restored.orbit.hpop, satellite.orbit.hpop)
                    self.assertEqual(restored.orbit.hpop.step_control, "fixed")
                    self.assertEqual(loaded.prepare().state(restored, at=loaded.last).position_m, state.position_m)
                self.assertEqual(first.state(satellite, at=project.last).position_m, state.position_m)
                satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop,
                    step_control="relative_error", relative_tolerance=1e-12,
                    position_tolerance_m=.001, velocity_tolerance_mps=1e-6,
                    min_step_seconds=.001, max_step_seconds=60))
                switched = project.prepare().states(satellite, step=u.s(600))
                if method in ("rk4", "gauss_jackson"):
                    self.assertEqual(switched.input_sha256, captured.input_sha256)
                    self.assertEqual(switched.position.values, captured.position.values)
                else:
                    self.assertNotEqual(switched.input_sha256, captured.input_sha256)
        with self.assertRaises(ValueError):
            self.make(step_control="automatic")

    def test_full_day_steps_roundtrip_and_active_identity(self):
        radius = 1e10
        for method in ("rk4", "rkf78", "rkv89", "bulirsch_stoer", "gauss_jackson"):
            for control in ("fixed", "relative_error"):
                with self.subTest(method=method, control=control):
                    project = of.Project("Long steps", of.Epoch(0), of.Epoch(259200 * 10**9))
                    initial = of.orbits.Cartesian(of.Epoch(0), (radius, 0, 0),
                                                  (0, math.sqrt(3.986004415e14 / radius), 0))
                    satellite = project.objects.satellite("Long", orbit=of.orbits.Hpop(
                        initial=initial, gravity_degree=0, integrator=method, step_control=control,
                        min_step_seconds=86400, max_step_seconds=86400, fixed_step_seconds=86400))
                    analysis = project.prepare()
                    result = analysis.states(satellite, step=u.s(86400))
                    actual = analysis.state(satellite, at=project.last)
                    angle = math.sqrt(3.986004415e14 / radius**3) * 259200
                    self.assertLess(math.dist(actual.position_m, (radius*math.cos(angle),
                                                                  radius*math.sin(angle), 0)), .005)
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory) / "full-day.orbitforge"
                        project.save(path, analysis=analysis)
                        reopened = of.Project.open(path)
                        restored = reopened.objects[satellite.id]
                        self.assertEqual(restored.orbit.hpop, satellite.orbit.hpop)
                        self.assertEqual(reopened.prepare().state(restored, at=project.last).position_m,
                                         actual.position_m)
                    settings = satellite.orbit.hpop
                    if settings._native().uses_fixed_step():
                        changed = replace(settings, fixed_step_seconds=43200)
                    else:
                        changed = replace(settings, min_step_seconds=43200, max_step_seconds=43200)
                    satellite.orbit = replace(satellite.orbit, hpop=changed)
                    self.assertNotEqual(project.prepare().states(satellite, step=u.s(86400)).input_sha256,
                                        result.input_sha256)
                    self.assertEqual(analysis.state(satellite, at=project.last).position_m, actual.position_m)
        for option in ("min_step_seconds", "max_step_seconds", "fixed_step_seconds"):
            with self.assertRaises(ValueError):
                self.make(**{option: 86400.1})

    def test_easy_constructor_copy_edit_and_current_file_roundtrip(self):
        project, satellite = self.make(gravity_degree=0, max_step_seconds=45)
        settings = satellite.orbit.hpop
        with self.assertRaises(FrozenInstanceError):
            settings.max_step_seconds = 15
        analysis = project.prepare()
        before = analysis.state(satellite, at=project.last)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "numerical.orbitforge"
            project.save(path, analysis=analysis)
            loaded = of.Project.open(path)
            restored = loaded.objects[satellite.id]
            self.assertEqual(restored.orbit.hpop, settings)
            self.assertEqual(loaded.prepare().state(restored, at=project.last).position_m, before.position_m)
        satellite.orbit = replace(satellite.orbit, hpop=replace(settings, integrator="rk4", fixed_step_seconds=5))
        self.assertNotEqual(project.prepare().state(satellite, at=project.last).position_m, before.position_m)
        self.assertEqual(analysis.state(satellite, at=project.last).position_m, before.position_m)

    def test_adaptive_public_options_identity_snapshots_and_project_roundtrip(self):
        for integrator, native_integrator in (("rkv89", of.HpopIntegrator.RKV89),
                                              ("bulirsch_stoer", of.HpopIntegrator.BULIRSCH_STOER)):
            project, satellite = self.make(gravity_degree=0)
            original = project.prepare()
            original_states = original.states(satellite, step=u.s(600))
            satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, integrator=integrator))
            settings = satellite.orbit.hpop
            self.assertEqual(settings.integrator, integrator)
            self.assertEqual(settings._native().integrator, native_integrator)
            analysis = project.prepare()
            result = analysis.states(satellite, step=u.s(600))
            self.assertNotEqual(result.input_sha256, original_states.input_sha256)
            self.assertEqual(original.states(satellite, step=u.s(600)).position.values, original_states.position.values)
            for point, reference in zip(result.position.values, original_states.position.values):
                self.assertLess(math.dist(point, reference), .01)
            satellite.orbit = replace(satellite.orbit, hpop=replace(settings, fixed_step_seconds=7))
            self.assertEqual(project.prepare().states(satellite, step=u.s(600)).input_sha256, result.input_sha256)
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "adaptive.orbitforge"
                project.save(path)
                restored = of.Project.open(path)
                sat = restored.objects[satellite.id]
                self.assertEqual(sat.orbit.hpop.integrator, integrator)
                self.assertEqual(restored.prepare().states(sat, step=u.s(600)).position.values, result.position.values)
            _, explicit = self.make(gravity_degree=0, integrator=integrator)
            self.assertEqual(explicit.orbit.hpop.integrator, integrator)

    def test_gauss_jackson_corrections_active_identity_and_roundtrip(self):
        for correction, native in (("full", of.HpopCorrection.FULL), ("pseudo", of.HpopCorrection.PSEUDO)):
            project, satellite = self.make(gravity_degree=0, integrator="gauss_jackson",
                                           fixed_step_seconds=30, correction=correction)
            settings = satellite.orbit.hpop
            self.assertEqual(settings._native().integrator, of.HpopIntegrator.GAUSS_JACKSON)
            self.assertEqual(settings._native().correction, native)
            prepared = project.prepare()
            original = prepared.states(satellite, step=u.s(600))
            satellite.orbit = replace(satellite.orbit, hpop=replace(settings,
                max_step_seconds=100, relative_tolerance=1e-6))
            inactive = project.prepare().states(satellite, step=u.s(600))
            self.assertEqual(inactive.input_sha256, original.input_sha256)
            self.assertEqual(inactive.position.values, original.position.values)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder)/"gauss-jackson.orbitforge"
                project.save(path)
                reopened = of.Project.open(path)
                restored = reopened.objects[satellite.id]
                self.assertEqual(restored.orbit.hpop.correction, correction)
                self.assertEqual(reopened.prepare().states(restored, step=u.s(600)).position.values,
                                 original.position.values)
            other = "pseudo" if correction == "full" else "full"
            satellite.orbit = replace(satellite.orbit, hpop=replace(settings, correction=other))
            switched = project.prepare().states(satellite, step=u.s(600))
            self.assertNotEqual(switched.input_sha256, original.input_sha256)
            self.assertEqual(prepared.states(satellite, step=u.s(600)).position.values, original.position.values)
            for a,b in zip(switched.position.values, original.position.values):
                self.assertLess(math.dist(a,b), 1e-4)
            satellite.orbit = replace(satellite.orbit, hpop=replace(settings, fixed_step_seconds=15))
            self.assertNotEqual(project.prepare().states(satellite, step=u.s(600)).input_sha256, original.input_sha256)
        project, satellite = self.make(gravity_degree=0)
        original = project.prepare().states(satellite, step=u.s(600))
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, correction="pseudo"))
        self.assertEqual(project.prepare().states(satellite, step=u.s(600)).input_sha256, original.input_sha256)
        with self.assertRaisesRegex(ValueError, "correction"):
            self.make(gravity_degree=0, correction="invalid")

    def test_mass_is_shared_validated_and_persisted_without_changing_spherical_ratios(self):
        project, satellite = self.make(gravity_degree=0, satellite_mass_kg=1250,
            srp=of.orbits.SrpSettings(area_mass_ratio_m2_per_kg=.03))
        settings = satellite.orbit.hpop
        self.assertEqual(settings.drag_area_m2, 25)
        self.assertEqual(settings.srp_area_m2, 37.5)
        native = settings._native()
        self.assertEqual(native.satellite_mass_kg, 1250)
        self.assertEqual(native.srp_area_m2, 37.5)
        with self.assertRaises(AttributeError):
            native.drag_area_m2 = 1
        analysis = project.prepare()
        before = analysis.states(satellite, step=u.s(600))
        satellite.orbit = replace(satellite.orbit, hpop=replace(settings, satellite_mass_kg=2500))
        after = project.prepare().states(satellite, step=u.s(600))
        self.assertEqual(satellite.orbit.hpop.drag_area_m2, 50)
        self.assertEqual(satellite.orbit.hpop.srp_area_m2, 75)
        self.assertEqual(after.input_sha256, before.input_sha256)
        self.assertEqual(after.position.values, before.position.values)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mass.orbitforge"
            project.save(path, analysis=analysis)
            reopened = of.Project.open(path)
            self.assertEqual(reopened.objects[satellite.id].orbit.hpop, satellite.orbit.hpop)
        revision = project.revision
        for mass in (0, -1, float("nan"), float("inf"), 5e-324):
            with self.subTest(mass=mass), self.assertRaises(ValueError):
                satellite.orbit = replace(satellite.orbit, hpop=replace(settings, satellite_mass_kg=mass))
        self.assertEqual(project.revision, revision)
        self.assertEqual(satellite.orbit.hpop.satellite_mass_kg, 2500)
        self.assertEqual(analysis.states(satellite, step=u.s(600)).position.values, before.position.values)

    def test_output_grid_is_independent_and_keplerian_uses_egm96_mu(self):
        project, satellite = self.make(gravity_degree=0)
        analysis = project.prepare()
        dense = analysis.states(satellite, step=u.s(30))
        sparse = project.prepare().states(satellite, step=u.s(60))
        self.assertEqual(dense.position.values[::2], sparse.position.values)
        elements = of.orbits.Keplerian(of.Epoch(0), u.km(7000), 0, u.deg(0))
        initial = of.orbits.Hpop(initial=elements, gravity_degree=0).initial
        self.assertAlmostEqual(initial.velocity_mps[1], math.sqrt(3.986004415e14 / 7e6), delta=1e-10)

    def test_harmonics_need_data_even_in_gcrf_and_keep_its_hash(self):
        project, satellite = self.make()
        with self.assertRaisesRegex(ValueError, "HPOP.*EOP"):
            project.prepare()
        data = of.DataSnapshot.load(eop_manifest=EOP)
        analysis = project.prepare(data=data)
        result = analysis.states(satellite, step=u.s(60))
        self.assertEqual(result.data_sha256, data.data_lock.sha256)
        station = project.objects.ground_station("Station", location=of.Wgs84Location(u.deg(0), u.deg(0)))
        analysis = project.prepare(data=data)
        self.assertEqual(len(analysis.aer(station, satellite, step=u.s(60))), 121)
        satellite.orbit = replace(satellite.orbit, initial=replace(satellite.orbit.initial, epoch=of.Epoch(-10 * 86400 * 10**9)))
        with self.assertRaisesRegex(of.EopCoverageError, "initial epoch"):
            project.prepare(data=data)

    def test_invalid_options_fail_before_mutating_objects(self):
        project, satellite = self.make(gravity_degree=0)
        revision = project.revision
        for bad in (dict(gravity_degree=1), dict(gravity_degree=21), dict(gravity_order=21),
                    dict(integrator="bad"), dict(max_step_seconds=0), dict(relative_tolerance=float("nan")),
                    dict(position_tolerance_m=-1), dict(minimum_altitude_m=-1)):
            with self.subTest(options=bad), self.assertRaises(ValueError):
                satellite.orbit = of.orbits.Hpop(initial=satellite.orbit.initial, **bad)
        self.assertEqual(project.revision, revision)


if __name__ == "__main__":
    unittest.main()
