from dataclasses import FrozenInstanceError, replace
from pathlib import Path
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u

ROOT = Path(__file__).resolve().parents[3]
FILE = ROOT / "tests/reference/propagators/ocean_fes2014bv1_4x4.pot"
EOP = ROOT / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"


class OceanTideTests(unittest.TestCase):
    def make(self, tides):
        p = of.Project("Ocean tides", of.Epoch(0), of.Epoch(600 * 10**9))
        sat = p.objects.satellite("Sat", orbit=of.orbits.Hpop(
            initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
            gravity_degree=0, ocean_tides=tides))
        return p, sat

    def test_options_are_immutable_validated_and_inactive_drafts_roundtrip(self):
        tides = of.orbits.OceanTideSettings(False, "missing", 100, 12)
        with self.assertRaises(FrozenInstanceError):
            tides.degree = 2
        for kwargs in ({"enabled": 1}, {"degree": 4.5}, {"order": True}, {"degree": -1},
                       {"degree": 2191}, {"order": 5}, {"model": "../bad"}, {"model": ""}):
            with self.subTest(kwargs=kwargs), self.assertRaises((ValueError, TypeError)):
                of.orbits.OceanTideSettings(**kwargs)
        p, sat = self.make(tides)
        analysis = p.prepare()
        before = analysis.states(sat, step=u.s(60))
        sat.orbit = replace(sat.orbit, hpop=replace(sat.orbit.hpop,
            ocean_tides=of.orbits.OceanTideSettings(True, "unused", 1, 1)))
        after = p.prepare().states(sat, step=u.s(60))
        self.assertEqual(before.input_sha256, after.input_sha256)
        self.assertEqual(before.position.values, after.position.values)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "inactive.orbitforge"
            p.save(path, analysis=analysis)
            reopened = of.Project.open(path)
            self.assertEqual(reopened.objects[sat.id].orbit.hpop.ocean_tides,
                             sat.orbit.hpop.ocean_tides)

    def test_catalog_snapshot_reports_and_changed_data_share_the_same_model(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            catalog = of.DataCatalog(root / "catalog")
            try:
                entry = catalog.install_ocean_tides(FILE, "custom-ocean")
                self.assertEqual((entry.info.degree, entry.info.order, entry.info.constituents), (4, 4, 36))
                self.assertEqual(entry.info.sha256, of.load_ocean_tides(FILE).info.sha256)
                self.assertEqual(catalog.ocean_tide_models[0].selection, "custom-ocean")
                with self.assertRaises(AttributeError):
                    entry.info.degree = 8
                with self.assertRaises(RuntimeError):
                    catalog.install_ocean_tides(FILE)
                catalog.install_eop(EOP)
                settings = of.orbits.OceanTideSettings(True, "custom-ocean", 4, 2)
                p, sat = self.make(settings)
                with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing ocean"):
                    p.prepare(data=of.DataSnapshot.load(eop_manifest=EOP))
                data = of.DataSnapshot.load(eop_manifest=EOP).with_ocean_tides("custom-ocean", FILE)
                frozen = p.prepare(data=data)
                before = frozen.states(sat, step=u.s(60))
                self.assertIn("ocean-tides=iers2010", before.provenance.algorithm)
                from_catalog = p.prepare(data=catalog).states(sat, step=u.s(60))
                self.assertEqual(from_catalog.position.values, before.position.values)
                self.assertEqual(from_catalog.data_sha256, before.data_sha256)
                frozen.report(of.ReportKind.CARTESIAN_STATE, objects=[sat], step=u.s(60))
                path = root / "ocean.orbitforge"
                p.save(path, analysis=frozen)
                reopened = of.Project.open(path, data=data)
                self.assertEqual(reopened.objects[sat.id].orbit.hpop.ocean_tides, settings)
                source = root / "changed.pot"
                source.write_bytes(FILE.read_bytes().replace(b"4.34727", b"8.34727"))
                changed = data.with_ocean_tides("custom-ocean", source)
                plan = of.Project.plan_open(path, data=changed)
                self.assertEqual(plan.data_changes[0].role, "ocean-tides/custom-ocean")
                with self.assertRaises(of.DataChangedError):
                    plan.open()
                current = plan.open(accept_data_changes=True)
                after = current.prepare(data=changed).states(current.objects[sat.id], step=u.s(60))
                self.assertEqual(before.input_sha256, after.input_sha256)
                self.assertNotEqual(before.data_sha256, after.data_sha256)
                self.assertNotEqual(before.position.values[-1], after.position.values[-1])
                self.assertEqual(frozen.states(sat, step=u.s(60)).position.values, before.position.values)
                too_large, _ = self.make(replace(settings, degree=5))
                with self.assertRaisesRegex(ValueError, "truncation"):
                    too_large.prepare(data=data)
            finally:
                del catalog
