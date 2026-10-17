"""Independent numerical reference, shared runtime and immutable saved relativity settings."""
import json
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path
import orbitforge as of
from orbitforge import units as u

ROOT = Path(__file__).resolve().parents[3]
EOP = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
SPICE = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"

class RelativityTests(unittest.TestCase):
    def test_native_terms_match_independent_orekit_reference(self):
        rows = 0
        for line in (ROOT / "tests/reference/propagators/relativity.txt").read_text().splitlines():
            if not line.startswith("A "):
                continue
            values = [float(x) for x in line.split()[1:]]
            result = of.evaluate_relativity(values[:3], values[3:6], 3.986004415e14,
                values[6:9], values[9:12], 1.3271244004127942e20, values[12:15])
            for i, actual in enumerate((result.schwarzschild_mps2, result.lense_thirring_mps2,
                                        result.de_sitter_mps2, result.acceleration_mps2)):
                for a, b in zip(actual, values[15+3*i:18+3*i]):
                    self.assertAlmostEqual(a, b, delta=2e-22 if i in (0, 3) else 2e-24)
            rows += 1
        self.assertEqual(rows, 45)
        with self.assertRaises(ValueError):
            of.evaluate_relativity((0, 0, 0), (0, 7000, 0), 3.986e14,
                                  (1.5e11, 0, 0), (0, 30000, 0), 1.327e20, (0, 0, 1))

    def test_settings_are_strict_frozen_and_roundtrip(self):
        self.assertFalse(of.HpopOptions().include_relativistic_accelerations)
        for invalid in (1, None, "true"):
            with self.assertRaises(TypeError):
                of.orbits.HpopSettings(include_relativistic_accelerations=invalid)
        for enabled in (False, True):
            setting = of.orbits.HpopSettings(include_relativistic_accelerations=enabled)
            self.assertEqual(of.orbits.HpopSettings._read(setting._native()), setting)
            with self.assertRaises(FrozenInstanceError):
                setting.include_relativistic_accelerations = False
        orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
                              gravity_degree=0, include_relativistic_accelerations=True)
        self.assertTrue(orbit.hpop.include_relativistic_accelerations)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), "requires CSPICE/ERFA")
    def test_relativistic_point_mass_requires_inputs_and_preserves_saved_analysis_and_reports(self):
        first = of.Epoch(json.loads(SPICE.read_text())["first_tai_ns"] + 3600 * 10**9)
        last = of.Epoch(first.tai_nanoseconds + 1200 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = of.DataSnapshot.load(eop_manifest=EOP).with_ephemeris("de440s", SPICE, root / "runtime")
            without_frames = of.DataSnapshot.builtin_time().with_ephemeris("de440s", SPICE, root / "runtime")
            without_ephemeris = of.DataSnapshot.load(eop_manifest=EOP)
            project = of.Project("Relativity", first, last)
            orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                                  gravity_degree=0, include_relativistic_accelerations=True)
            satellite = project.objects.satellite("Sat", orbit=orbit)
            with self.assertRaises((of.AnalysisCapabilityError, ValueError)):
                project.prepare(data=without_frames)
            with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing ephemeris"):
                project.prepare(data=without_ephemeris)
            analysis = project.prepare(data=data)
            states = analysis.states(satellite, step=u.s(60))
            report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(60),
                                     units=of.ReportUnits.SI, time_scale=of.ReportTimeScale.TAI)
            self.assertEqual(tuple(report.table.rows[-1][4:7]), states.position.values[-1])
            self.assertEqual(satellite.orbit.hpop.third_bodies, ())
            path = root / "relativity.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            self.assertTrue(restored.objects[satellite.id].orbit.hpop.include_relativistic_accelerations)
            restored_states = restored.prepare(data=data).states(restored.objects[satellite.id], step=u.s(60))
            self.assertEqual(restored_states.position.values, states.position.values)
            self.assertEqual(restored_states.input_sha256, states.input_sha256)
            satellite.orbit = replace(orbit, hpop=replace(orbit.hpop, include_relativistic_accelerations=False))
            other = project.prepare().states(satellite, step=u.s(60))
            self.assertNotEqual(other.input_sha256, states.input_sha256)
            self.assertNotEqual(other.position.values[-1], states.position.values[-1])
            self.assertEqual(analysis.states(satellite, step=u.s(60)).position.values, states.position.values)
            with self.assertRaises(of.RevisionConflictError):
                project.save(root / "stale.orbitforge", analysis=analysis)
            del analysis, data, without_frames, restored

if __name__ == "__main__":
    unittest.main()
