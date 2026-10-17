"""Owned planetary SPK imports, numerical reference and public HPOP persistence."""
from pathlib import Path
import hashlib
import json
import shutil
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u

SAMPLES = Path(__file__).resolve().parents[3] / "data/samples/spice"
BASE = SAMPLES / "de440s_planets_2016_leap/manifest.json"
MARS = SAMPLES / "mar099s_2016_leap/mar099s_subset.bsp"


@unittest.skipUnless(of.ephemeris_available() and of.data_catalog_available(), "requires CSPICE/catalog")
class PlanetaryCentresTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_import_reference_frozen_hpop_saved_project_and_data_changes(self):
        catalog = second = raw = data = old = analysis = restored = changed = plan = accepted = None
        try:
            catalog = of.DataCatalog(self.root / "catalog")
            base = catalog.install_ephemeris_snapshot(BASE)
            entry = catalog.install_planetary_centres(MARS, "urn:orbitforge:validation:mar099s-subset")
            self.assertNotEqual(base.info.content_sha256, entry.info.content_sha256)
            self.assertEqual(catalog.install_planetary_centres(MARS, "urn:orbitforge:validation:mar099s-subset").info.content_sha256,
                             entry.info.content_sha256)
            raw = of.load_spice_snapshot(entry.manifest, self.root / "queries")
            count = 0
            for line in (MARS.parent / "mars_jplephem.txt").read_text(encoding="utf-8").splitlines():
                if line.startswith("#"):
                    continue
                parts = line.split()
                epoch = of.Epoch(int(parts[0]))
                values = list(map(float, parts[1:]))
                local = raw.geometric_state(499, 4, epoch)
                earth = raw.geometric_state(499, 399, epoch)
                for actual, expected in zip(local.position_m, values[1:4]):
                    self.assertAlmostEqual(actual, expected, delta=1e-9)
                for actual, expected in zip(earth.position_m, values[7:10]):
                    self.assertAlmostEqual(actual, expected, delta=2e-4)
                count += 1
            self.assertEqual(count, 37)
            first = of.Epoch(entry.info.first.tai_nanoseconds + 3600 * 10**9)
            last = of.Epoch(first.tai_nanoseconds + 60 * 10**9)
            data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", entry.manifest, self.root / "runtime")
            old = of.DataSnapshot.builtin_time().with_ephemeris("de440s", base.manifest, self.root / "old-runtime")
            project = of.Project("Mars shadow", first, last)
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Cartesian(first,(7e6,0,0),(0,7500,0)), gravity_degree=0,
                srp=of.orbits.SrpSettings(enabled=True,additional_eclipsing_bodies=(of.orbits.SrpEclipsingBody("mars"),))))
            with self.assertRaises(of.EphemerisCoverageError):
                project.prepare(data=old)
            analysis = project.prepare(data=data)
            expected = analysis.states(sat,step=u.s(30)).position.values
            path = self.root / "mars.orbitforge"
            project.save(path,analysis=analysis)
            restored = of.Project.open(path,data=data)
            self.assertEqual(restored.prepare(data=data).states(restored.objects[sat.id],step=u.s(30)).position.values,expected)
            revised = catalog.install_planetary_centres(MARS,"urn:orbitforge:validation:revised-provenance")
            changed = of.DataSnapshot.builtin_time().with_ephemeris("de440s",revised.manifest,self.root / "changed-runtime")
            plan = of.Project.plan_open(path,data=changed)
            self.assertEqual(plan.data_changes[0].role,"ephemeris/de440s")
            with self.assertRaises(of.DataChangedError):
                plan.open()
            accepted = plan.open(accept_data_changes=True)
            self.assertEqual(accepted.prepare(data=changed).states(accepted.objects[sat.id],step=u.s(30)).position.values,expected)
            self.assertEqual(analysis.states(sat,step=u.s(30)).position.values,expected)
            with self.assertRaises(of.EphemerisCoverageError):
                project.prepare(data=old)
            # A catalogue copy is self-contained and survives losing the source package.
            second = of.DataCatalog(self.root / "second-catalog")
            imported = second.install_ephemeris_snapshot(entry.manifest)
            self.assertEqual(imported.info.content_sha256,entry.info.content_sha256)
        finally:
            del catalog, second, raw, data, old, analysis, restored, changed, plan, accepted

    def test_forged_supplement_role_path_escape_and_invalid_source_preserve_receipt(self):
        catalog = None
        try:
            catalog = of.DataCatalog(self.root / "catalog")
            with self.assertRaises(RuntimeError):
                catalog.install_planetary_centres(MARS)
            entry = catalog.install_ephemeris_snapshot(BASE)
            folder = self.root / "bad-snapshot"
            shutil.copytree(BASE.parent,folder)
            body = json.loads((folder / "manifest.json").read_text())
            # The original Mars kernel also contains Sun/Earth/system segments.
            # It is not valid as a supplemental role even if its hash is correct.
            shutil.copy2(MARS,folder / "centre-499.bsp")
            checksum = hashlib.sha256(MARS.read_bytes()).hexdigest()
            body["kernels"].append(dict(file="centre-499.bsp",kind="SPK_BODY_CENTRES",sha256=checksum,
                                        source_sha256=checksum,source_url="synthetic-test",producer="test"))
            manifest = folder / "manifest.json"
            manifest.write_text(json.dumps(body),encoding="utf-8")
            with self.assertRaises((of.EphemerisDataError,RuntimeError)):
                catalog.install_ephemeris_snapshot(manifest)
            self.assertEqual(catalog.ephemerides[0].info.content_sha256,entry.info.content_sha256)
            body["kernels"][-1]["kind"] = "SPK"
            manifest.write_text(json.dumps(body),encoding="utf-8")
            with self.assertRaises(of.EphemerisDataError):
                catalog.install_ephemeris_snapshot(manifest)
            body["kernels"][-1]["file"] = "../outside.bsp"
            manifest.write_text(json.dumps(body),encoding="utf-8")
            with self.assertRaises(RuntimeError):
                catalog.install_ephemeris_snapshot(manifest)
            self.assertEqual(catalog.ephemerides[0].info.content_sha256,entry.info.content_sha256)
            invalid = self.root / "bad.bsp"
            invalid.write_bytes(b"not a binary SPK")
            with self.assertRaises((RuntimeError,of.EphemerisQueryError,of.EphemerisDataError)):
                catalog.install_planetary_centres(invalid)
            self.assertEqual(catalog.ephemerides[0].info.content_sha256,entry.info.content_sha256)
            self.assertEqual(list((self.root / "catalog/staging").iterdir()),[])
        finally:
            del catalog


if __name__ == "__main__":
    unittest.main()
