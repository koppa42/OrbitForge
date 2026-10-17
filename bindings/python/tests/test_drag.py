"""Density references, shared drag configuration, retained UTC and immutable results."""
import json
import math
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path

import orbitforge as of
from orbitforge import units as u

ROOT = Path(__file__).resolve().parents[3]
EOP = Path(of.__file__).parent / "data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
SPICE = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"


class DragTests(unittest.TestCase):
    def test_settings_ranges_and_independent_density_grid(self):
        settings = of.orbits.DragSettings(enabled=True, atmosphere="nrlmsise00")
        with self.assertRaises(FrozenInstanceError):
            settings.cd = 3
        for kwargs in ({"enabled": 1}, {"use_apparent_sun_position": "true"},
                       {"use_approximate_altitude": 1},
                       {"cd": float("inf")}, {"area_mass_ratio_m2_per_kg": -1},
                       {"atmosphere": "unknown"}, {"kp": -1}, {"kp": 9.1}, {"f107": 0}):
            with self.assertRaises((TypeError, ValueError)):
                of.orbits.DragSettings(**kwargs)
        self.assertEqual(of.kp_to_ap(3), 15)
        for model, name in ((of.AtmosphereModel.JACCHIA_ROBERTS, "jacchia_roberts_precise.txt"),
                            (of.AtmosphereModel.NRLMSISE00, "nrlmsise00.txt")):
            rows = [line.split() for line in (ROOT / "tests/reference/propagators" / name)
                    .read_text().splitlines() if line and not line.startswith("#")]
            for row in rows[::37]:
                values = list(map(float, row))
                year, month, day = map(int, values[:3])
                second = int(values[3])
                hour, minute, sec = (23, 59, 60) if second == 86400 else (
                    second // 3600, second % 3600 // 60, second % 60)
                inputs = of.AtmosphereInput()
                inputs.utc = of.UtcDateTime(year, month, day, hour, minute, sec)
                inputs.geodetic = (values[6], values[5], values[4])
                inputs.solar_hour_angle_rad, inputs.sun_declination_rad = values[7:9]
                inputs.f107, inputs.f107_average, inputs.kp = values[9:12]
                actual = of.evaluate_atmosphere(model, inputs)
                self.assertAlmostEqual(actual.density_kg_m3 / values[12], 1, delta=2e-10)

    def test_high_altitude_roberts_matches_high_precision_original_equations(self):
        source = ROOT / "tests/reference/propagators/drag_density_high.txt"
        rows = [list(map(float, line.split())) for line in source.read_text().splitlines()
                if line and not line.startswith("#")]
        self.assertEqual(len(rows), 216)
        for row in rows:
            mode, approximate = map(int, row[:2])
            options = of.DragOptions()
            options.enabled = True
            options.use_approximate_altitude = bool(approximate)
            options.f107, options.f107_average, options.kp = (
                (150, 150, 3), (180, 165, 5.5), (70, 80, 0))[mode]
            actual = of.evaluate_drag(row[2:5], (1234, -6234, 3456), (1.4e11, -5e10, 3e10),
                                      of.UtcDateTime(2000, 2, 29, 13, 23, 12, 500000000), options)
            with self.subTest(mode=mode, approximate=approximate, position=row[2:5]):
                self.assertAlmostEqual(actual.density_kg_m3 / row[5], 1, delta=3e-11)

    def test_explicit_daily_and_history_ap_match_author_reference(self):
        rows = (ROOT / "tests/reference/propagators/nrlmsise00_ap.txt").read_text().splitlines()
        rows = [list(map(float, line.split())) for line in rows if line and not line.startswith("#")]
        self.assertEqual(len(rows), 1056)
        for row in rows[::17]:
            inputs = of.AtmosphereInput()
            seconds = int(row[3])
            hour, minute, second = (23, 59, 60) if seconds == 86400 else (
                seconds // 3600, seconds % 3600 // 60, seconds % 60)
            inputs.utc = of.UtcDateTime(*map(int, row[:3]), hour, minute, second)
            inputs.geodetic = (row[6], row[5], row[4])
            inputs.solar_hour_angle_rad = row[7]
            inputs.f107, inputs.f107_average = row[8:10]
            inputs.kp = float("nan")  # Explicit Ap leaves Kp inactive.
            if row[10]:
                inputs.ap_history = row[11:18]
            else:
                inputs.daily_ap = row[11]
            result = of.evaluate_atmosphere(of.AtmosphereModel.NRLMSISE00, inputs)
            self.assertAlmostEqual(result.density_kg_m3 / row[18], 1, delta=2e-10)
            self.assertAlmostEqual(result.temperature_k, row[19], delta=1e-7)
            self.assertAlmostEqual(result.exospheric_temperature_k, row[20], delta=1e-7)

    def test_explicit_ap_validation_selection_and_owned_values(self):
        inputs = of.AtmosphereInput()
        inputs.geodetic = (.4, .6, 400000)
        model = of.AtmosphereModel.NRLMSISE00
        self.assertIsNone(inputs.daily_ap)
        self.assertIsNone(inputs.ap_history)
        manual = of.evaluate_atmosphere(model, inputs).density_kg_m3
        inputs.daily_ap = 15
        self.assertEqual(of.evaluate_atmosphere(model, inputs).density_kg_m3, manual)
        history = [15.0] * 7
        inputs.ap_history = history
        with self.assertRaises(ValueError):
            of.evaluate_atmosphere(model, inputs)
        inputs.daily_ap = None
        original = of.evaluate_atmosphere(model, inputs).density_kg_m3
        self.assertNotEqual(original, manual)
        history[1] = 400
        returned = inputs.ap_history
        returned[2] = 400
        self.assertEqual(inputs.ap_history, [15.0] * 7)
        self.assertEqual(of.evaluate_atmosphere(model, inputs).density_kg_m3, original)
        with self.assertRaises(ValueError):
            of.evaluate_atmosphere(of.AtmosphereModel.JACCHIA_ROBERTS, inputs)
        for wrong_length in ([1] * 6, [1] * 8):
            with self.assertRaises(TypeError):
                inputs.ap_history = wrong_length
        for bad in (-1, 400.01, float("nan"), float("inf")):
            for index in range(7):
                values = [15.0] * 7
                values[index] = bad
                inputs.ap_history = values
                with self.assertRaises(ValueError):
                    of.evaluate_atmosphere(model, inputs)
            inputs.ap_history = None
            inputs.daily_ap = bad
            with self.assertRaises(ValueError):
                of.evaluate_atmosphere(model, inputs)
            inputs.daily_ap = None
        self.assertEqual(of.evaluate_atmosphere(model, inputs).density_kg_m3, manual)

    def test_approximate_altitude_changes_density_without_relabeling_geodetic_height(self):
        options = of.DragOptions()
        options.enabled = True
        utc = of.UtcDateTime(2000, 1, 1, 12, 0, 0)
        args = ((4_000_000, 3_000_000, 4_500_000), (0, 7200, 0), (1e11, 0, 2e10), utc)
        exact = of.evaluate_drag(*args, options)
        self.assertEqual(exact.density_altitude_m, exact.geodetic[2])
        options.use_approximate_altitude = True
        approximate = of.evaluate_drag(*args, options)
        self.assertAlmostEqual(approximate.density_altitude_m, 358271.6089933367, delta=3e-9)
        self.assertEqual(approximate.geodetic, exact.geodetic)
        self.assertGreater(approximate.density_altitude_m, exact.geodetic[2])
        self.assertNotEqual(approximate.density_kg_m3, exact.density_kg_m3)
        self.assertNotEqual(approximate.acceleration_itrf_mps2, exact.acceleration_itrf_mps2)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), "requires CSPICE/ERFA")
    def test_shared_mass_preserves_active_force_identity_and_captured_analysis(self):
        metadata = json.loads(SPICE.read_text())
        first = of.Epoch(metadata["first_tai_ns"] + 3600 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = analysis = reopened = None
            try:
                data = of.DataSnapshot.load(eop_manifest=EOP).with_ephemeris("de440s", SPICE, root / "runtime")
                project = of.Project("Mass", first, of.Epoch(first.tai_nanoseconds + 600 * 10**9))
                orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(first, (6778137, 0, 0), (0, 6300, 4300)),
                    gravity_degree=0, drag=of.orbits.DragSettings(enabled=True, atmosphere="nrlmsise00"),
                    srp=of.orbits.SrpSettings(enabled=True, area_mass_ratio_m2_per_kg=.03))
                sat = project.objects.satellite("Sat", orbit=orbit)
                analysis = project.prepare(data=data)
                before = analysis.states(sat, step=u.s(60))
                sat.orbit = replace(orbit, hpop=replace(orbit.hpop, satellite_mass_kg=250))
                after = project.prepare(data=data).states(sat, step=u.s(60))
                self.assertEqual(sat.orbit.hpop.drag_area_m2, 5)
                self.assertEqual(sat.orbit.hpop.srp_area_m2, 7.5)
                self.assertEqual(before.input_sha256, after.input_sha256)
                self.assertEqual(before.position.values, after.position.values)
                self.assertEqual(before.velocity.values, after.velocity.values)
                path = root / "mass.orbitforge"
                project.save(path, analysis=analysis)
                reopened = of.Project.open(path, data=data)
                self.assertEqual(reopened.objects[sat.id].orbit.hpop, sat.orbit.hpop)
                self.assertEqual(reopened.prepare(data=data).states(reopened.objects[sat.id], step=u.s(60))
                                 .position.values, before.position.values)
                sat.orbit = replace(sat.orbit, hpop=replace(sat.orbit.hpop,
                    drag=replace(sat.orbit.hpop.drag, area_mass_ratio_m2_per_kg=.04)))
                changed = project.prepare(data=data).states(sat, step=u.s(60))
                self.assertNotEqual(changed.input_sha256, before.input_sha256)
                self.assertNotEqual(changed.position.values[-1], before.position.values[-1])
                self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
            finally:
                del data, analysis, reopened

    def test_disabled_drafts_roundtrip_without_data_and_do_not_change_identity(self):
        project = of.Project("Drag drafts", of.Epoch(0), of.Epoch(600 * 10**9))
        drag = of.orbits.DragSettings(cd=2.3, f107=170, kp=4.3, use_apparent_sun_position=True,
                                     atmosphere="nrlmsise00", low_altitude_model="nrlmsise00",
                                     use_approximate_altitude=True)
        orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(of.Epoch(0), (6778137, 0, 0), (0, 6300, 4300)),
                              gravity_degree=0, drag=drag)
        sat = project.objects.satellite("Sat", orbit=orbit)
        before = project.prepare().states(sat, step=u.s(60))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "drag.orbitforge"
            project.save(path)
            reopened = of.Project.open(path)
            self.assertEqual(reopened.objects[sat.id].orbit.hpop.drag, drag)
        sat.orbit = replace(orbit, hpop=replace(orbit.hpop, drag=replace(drag, cd=2.9, kp=8)))
        after = project.prepare().states(sat, step=u.s(60))
        self.assertEqual(after.input_sha256, before.input_sha256)
        self.assertEqual(after.position.values, before.position.values)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), "requires CSPICE/ERFA")
    def test_drag_only_uses_eop_utc_and_sun_and_survives_report_save_edit_and_reopen(self):
        metadata = json.loads(SPICE.read_text())
        first = of.Epoch(metadata["first_tai_ns"] + 3600 * 10**9)
        last = of.Epoch(first.tai_nanoseconds + 1200 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            provider = of.load_spice_snapshot(SPICE, root / "direct")
            with self.assertRaises(of.EphemerisCoverageError):
                provider.validate_apparent_sun_interval(of.Epoch(metadata["first_tai_ns"]), first)
            apparent = provider.apparent_sun_from_earth(first)
            geometric = provider.geometric_state(10, 399, first).position_m
            self.assertGreater(math.dist(apparent, geometric), 1e6)
            time_only = of.DataSnapshot.builtin_time().with_ephemeris("de440s", SPICE, root / "runtime")
            data = of.DataSnapshot.load(eop_manifest=EOP).with_ephemeris("de440s", SPICE, root / "runtime")
            project = of.Project("Drag", first, last)
            orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(first, (6778137, 0, 0), (0, 6300, 4300)),
                                  gravity_degree=0,
                                  drag=of.orbits.DragSettings(enabled=True, use_apparent_sun_position=True,
                                                             use_approximate_altitude=True))
            sat = project.objects.satellite("Sat", orbit=orbit)
            with self.assertRaises((of.AnalysisCapabilityError, ValueError)):
                project.prepare(data=time_only)
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60))
            report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                                     units=of.ReportUnits.SI, time_scale=of.ReportTimeScale.TAI)
            self.assertEqual(tuple(report.table.rows[-1][4:7]), before.position.values[-1])
            path = root / "drag.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.drag, orbit.hpop.drag)
            self.assertEqual(restored.prepare(data=data).states(restored.objects[sat.id], step=u.s(60))
                             .position.values, before.position.values)
            for change in ({"cd": 2.8}, {"f107": 220}, {"kp": 7},
                           {"atmosphere": "nrlmsise00"}, {"use_apparent_sun_position": False},
                           {"use_approximate_altitude": False}):
                sat.orbit = replace(orbit, hpop=replace(orbit.hpop, drag=replace(orbit.hpop.drag, **change)))
                changed = project.prepare(data=data).states(sat, step=u.s(60))
                self.assertNotEqual(changed.input_sha256, before.input_sha256)
                self.assertNotEqual(changed.position.values[-1], before.position.values[-1])
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
            del analysis, data, time_only, restored, provider


if __name__ == "__main__":
    unittest.main()
