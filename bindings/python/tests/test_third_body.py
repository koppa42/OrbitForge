"""Sun/Moon configuration, immutable ephemerides and explicit update consent."""
import json
import shutil
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

import orbitforge as of
from orbitforge import units as u

SAMPLE = Path(__file__).resolve().parents[3] / "data/samples/spice/de440s_2016_leap"


class ThirdBodyTests(unittest.TestCase):
    def test_planet_sources_cb_capture_and_inactive_drafts_roundtrip(self):
        initial = of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Jupiter.cb"
            path.write_text("Gm 1.266865319e8\nSystemGm 1.26712764e8\n")
            cb = of.orbits.load_cb_gravity(path, "jupiter", mu_scale=1e9)
            self.assertEqual(cb.mu, 1.26712764e17)
            path.unlink()
            gravity = (of.orbits.ThirdBodySettings("jupiter", "cb_file", mu=1.1e17, cb=cb),
                       of.orbits.ThirdBodySettings("neptune", "user_specified", mu=6.8e15))
            orbit = of.orbits.Hpop(initial=initial, gravity_degree=0,
                                  third_bodies=("pluto", "jupiter", "venus", "mars", "mercury"),
                                  body_gravity=gravity)
            project = of.Project("Planets", of.Epoch(0), of.Epoch(600 * 10**9))
            sat = project.objects.satellite("Planets", orbit=orbit)
            self.assertEqual(sat.orbit.hpop, orbit.hpop)
            # Open input-only files without ephemerides while retaining inactive source drafts.
            orbit = replace(orbit, hpop=replace(orbit.hpop, third_bodies=()))
            sat.orbit = orbit
            scenario = Path(folder) / "scenario.orbitforge"
            project.save(scenario)
            restored = of.Project.open(scenario)
            self.assertEqual(restored.objects[sat.id].orbit.hpop, orbit.hpop)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.body_gravity[0].cb, cb)
            for source in ("cb_file", "user_specified", "invalid"):
                with self.assertRaises(ValueError):
                    of.orbits.Hpop(initial=initial, third_bodies=("jupiter",),
                                  body_gravity=(of.orbits.ThirdBodySettings("jupiter", source),))
            with self.assertRaises(ValueError):
                of.orbits.Hpop(initial=initial, third_bodies=("sun",),
                              body_gravity=(of.orbits.ThirdBodySettings("sun", "cb_file", cb=cb),))

    @unittest.skipUnless(of.ephemeris_available(), "requires CSPICE")
    def test_all_planets_prepare_and_missing_chains_fail(self):
        sample = SAMPLE.with_name("de440s_planets_2016_leap")
        metadata = json.loads((sample / "manifest.json").read_text())
        first = of.Epoch(metadata["first_tai_ns"])
        last = of.Epoch(first.tai_nanoseconds + 600 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", sample / "manifest.json", folder)
            project = of.Project("Solar system", first, last)
            sat = project.objects.satellite("All", orbit=of.orbits.Hpop(
                initial=of.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                gravity_degree=0, third_bodies=of.orbits.THIRD_BODY_NAMES))
            analysis = project.prepare(data=data)
            self.assertEqual(len(analysis.states(sat, step=u.s(60)).position.values), 11)
            missing = data.with_ephemeris("de440s", SAMPLE / "manifest.json", folder)
            with self.assertRaises(of.EphemerisCoverageError):
                project.prepare(data=missing)
            del analysis, data, missing

    def test_easy_settings_validate_normalize_and_roundtrip(self):
        initial = of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300))
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0,
                              third_bodies=["moon", "sun"], sun_mu=1.327e20)
        self.assertEqual(orbit.hpop.third_bodies, ("sun", "moon"))
        self.assertEqual(orbit.hpop.ephemeris, "de440s")
        project = of.Project("Bodies", of.Epoch(0), of.Epoch(600 * 10**9))
        sat = project.objects.satellite("Sat", orbit=orbit)
        self.assertEqual(sat.orbit.hpop, orbit.hpop)
        with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing ephemeris"):
            project.prepare()
        for bodies in (("earth",), ("sun", "sun"), "sun"):
            with self.assertRaises(ValueError):
                of.orbits.Hpop(initial=initial, third_bodies=bodies)
        with self.assertRaises(ValueError):
            of.orbits.Hpop(initial=initial, third_bodies=("sun",), sun_mu=0)

    @unittest.skipUnless(of.ephemeris_available(), "requires CSPICE")
    def test_spice_prepare_save_and_changed_data_requires_consent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shutil.copytree(SAMPLE, root / "ephemeris")
            manifest = root / "ephemeris/manifest.json"
            metadata = json.loads(manifest.read_text())
            first = of.Epoch(metadata["first_tai_ns"])
            last = of.Epoch(metadata["first_tai_ns"] + 600 * 10**9)
            data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", manifest, root / "runtime")
            project = of.Project("Third bodies", first, last)
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Keplerian(first, u.km(42164), 0, u.deg(15)),
                gravity_degree=0, third_bodies=("sun", "moon")))
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60)).position.values
            self.assertEqual(len(before), 11)
            path = root / "scenario.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.third_bodies, ("sun", "moon"))
            metadata["snapshot_id"] += "-changed"
            manifest.write_text(json.dumps(metadata))
            changed = data.with_ephemeris("de440s", manifest, root / "runtime")
            plan = of.Project.plan_open(path, data=changed)
            self.assertEqual(plan.data_changes[0].role, "ephemeris/de440s")
            with self.assertRaises(of.DataChangedError):
                plan.open()
            self.assertIsNotNone(plan.open(accept_data_changes=True))
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before)
            metadata["last_tai_ns"] = last.tai_nanoseconds - 1
            manifest.write_text(json.dumps(metadata))
            short = data.with_ephemeris("de440s", manifest, root / "runtime")
            with self.assertRaises(of.EphemerisCoverageError):
                of.Project.plan_open(path, data=short)
            # Release owned copies before removing their workspace on Windows.
            del analysis, data, changed, short, plan, restored


if __name__ == "__main__":
    unittest.main()
