"""Static file gravity shares immutable providers with propagation and saved projects."""
from dataclasses import replace
import json
import math
import tempfile
import unittest
from pathlib import Path
import orbitforge as of
from orbitforge import units as u

EOP = Path(__file__).resolve().parents[3] / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
GRV = """stk.v.6.0
Model Custom
Degree 21
Order 21
Gm 3.986004415e14
RefDistance 6378136.3
Normalized Yes
IncludesPermTide No
BEGIN Coefficients
2 0 -4.84165371736e-4 0
21 21 1e-8 2e-8
END Coefficients
"""

class GravityTests(unittest.TestCase):
    def test_install_select_prepare_roundtrip_and_update_consent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            file = root / "gravity.grv"
            file.write_text(GRV)
            catalog = of.DataCatalog(root / "catalog")
            receipt = catalog.install_gravity(file)
            self.assertEqual(receipt.info.degree, 21)
            self.assertEqual(receipt.selection, "custom")
            self.assertEqual(len(catalog.gravity_models), 1)
            data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", receipt.file)
            project = of.Project("Field", of.Epoch(0), of.Epoch(600 * 10**9))
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Keplerian(of.Epoch(0), u.km(7000), 0, u.deg(25)),
                gravity_model="custom", gravity_degree=21))
            with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing gravity"):
                project.prepare(data=of.DataSnapshot.load(eop_manifest=EOP))
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60)).position.values
            project.save(root / "project.orbitforge", analysis=analysis)
            restored = of.Project.open(root / "project.orbitforge", data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.gravity_model, "custom")
            file.write_text(GRV.replace("1e-8 2e-8", "2e-8 4e-8"))
            changed = data.with_gravity("custom", file)
            plan = of.Project.plan_open(root / "project.orbitforge", data=changed)
            self.assertEqual(plan.data_changes[0].role, "gravity/custom")
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before)
            with self.assertRaises(of.DataChangedError):
                plan.open()
            self.assertIsNotNone(plan.open(accept_data_changes=True))
            with self.assertRaises(RuntimeError):
                catalog.install_gravity(file, "egm2008")
            del catalog  # Close SQLite before TemporaryDirectory cleanup on Windows.

    def test_custom_love_number_metadata_roundtrip_changes_force_and_requires_data_consent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            file = root / "love.grv"
            custom = GRV.replace("BEGIN Coefficients", "Begin EarthIERSTideModel\n"
                "IncludesPermTide No\nLoveNumbers 2 0 .3 .12\nEnd EarthIERSTideModel\nBEGIN Coefficients")
            file.write_text(custom)
            catalog = of.DataCatalog(root / "catalog")
            try:
                receipt = catalog.install_gravity(file)
                self.assertEqual(receipt.info.solid_tide_k20, .3)
                self.assertEqual(catalog.gravity_models[0].info.solid_tide_k20, .3)
                with self.assertRaises(AttributeError):
                    receipt.info.solid_tide_k20 = .4
                data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", receipt.file)
                project = of.Project("Love numbers", of.Epoch(0), of.Epoch(600 * 10**9))
                sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                    initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
                    gravity_model="custom", gravity_degree=2,
                    solid_tides=of.orbits.SolidTideSettings("permanent")))
                analysis = project.prepare(data=data)
                before = analysis.states(sat, step=u.s(60))
                path = root / "love.orbitforge"
                project.save(path, analysis=analysis)
                restored = of.Project.open(path, data=data)
                self.assertEqual(restored.prepare(data=data).states(restored.objects[sat.id], step=u.s(60))
                                 .position.values, before.position.values)
                file.write_text(custom.replace(".3 .12", ".4 .12"))
                changed = data.with_gravity("custom", file)
                plan = of.Project.plan_open(path, data=changed)
                self.assertEqual(plan.data_changes[0].role, "gravity/custom")
                with self.assertRaises(of.DataChangedError):
                    plan.open()
                confirmed = plan.open(accept_data_changes=True)
                after = confirmed.prepare(data=changed).states(confirmed.objects[sat.id], step=u.s(60))
                self.assertEqual(after.input_sha256, before.input_sha256)
                self.assertNotEqual(after.data_sha256, before.data_sha256)
                for series, k20 in ((before, .3), (after, .4)):
                    used = series.provenance.algorithm.split(";k20=")[1].split(";")[0]
                    self.assertEqual(float.fromhex(used), k20)
                self.assertNotEqual(after.position.values[-1], before.position.values[-1])
                self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
                file.write_text(custom.replace("2 0 .3", "2 1 .3"))
                with self.assertRaises(RuntimeError):
                    catalog.install_gravity(file)
                self.assertEqual(catalog.gravity_models[0].info.solid_tide_k20, .3)
            finally:
                del catalog
            reopened = of.DataCatalog(root / "catalog")
            try:
                self.assertEqual(reopened.gravity_models[0].info.solid_tide_k20, .3)
            finally:
                del reopened

    def test_full_degree_rectangular_field_persists_and_enforces_actual_order(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            file = root / "full.grv"
            file.write_text(GRV.replace("Degree 21", "Degree 2190")
                            .replace("Order 21", "Order 2159")
                            .replace("21 21 1e-8 2e-8", "2190 2159 2e-8 -3e-8"))
            catalog = of.DataCatalog(root / "catalog")
            receipt = catalog.install_gravity(file)
            self.assertEqual((receipt.info.degree, receipt.info.order), (2190, 2159))
            data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", receipt.file)
            project = of.Project("Full field", of.Epoch(0), of.Epoch(2 * 10**9))
            satellite = project.objects.satellite("Full", orbit=of.orbits.Hpop(
                initial=of.orbits.Cartesian(of.Epoch(0), (6408136.3, 0, 0), (0, 6200, 4700)),
                gravity_model="custom", gravity_degree=2190, gravity_order=2159,
                max_step_seconds=1))
            settings = satellite.orbit.hpop
            revision = project.revision
            with self.assertRaises(ValueError):
                satellite.orbit = replace(satellite.orbit, hpop=replace(settings, gravity_degree=2191))
            self.assertEqual(project.revision, revision)
            analysis = project.prepare(data=data)
            before = analysis.state(satellite, at=project.last)
            self.assertTrue(all(math.isfinite(x) for x in before.position_m + before.velocity_mps))
            path = root / "full.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            restored_satellite = restored.objects[satellite.id]
            self.assertEqual(restored_satellite.orbit.hpop, settings)
            after = restored.prepare(data=data).state(restored_satellite, at=project.last)
            self.assertEqual(after.position_m, before.position_m)
            self.assertEqual(after.velocity_mps, before.velocity_mps)
            satellite.orbit = replace(satellite.orbit, hpop=replace(settings, gravity_order=2160))
            with self.assertRaises(ValueError):
                project.prepare(data=data)
            self.assertEqual(analysis.state(satellite, at=project.last).position_m, before.position_m)
            del catalog


class SecularGravityTests(unittest.TestCase):
    SOURCE = Path(__file__).resolve().parents[3] / "tests/reference/propagators/gravity_secular_0.grv"

    def test_explicit_metadata_project_roundtrip_and_changed_epoch_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            catalog = of.DataCatalog(root / "catalog")
            try:
                receipt = catalog.install_gravity(self.SOURCE)
                secular = receipt.info.secular_variations
                self.assertIsInstance(secular, of.GravitySecularVariations)
                self.assertEqual(secular.reference_epoch.tai_nanoseconds, -32_184_000_000)
                self.assertEqual(len(secular.rates_per_year), 4)
                self.assertEqual((secular.rates_per_year[0].degree, secular.rates_per_year[0].order), (2, 0))
                self.assertEqual(secular.rates_per_year[0].c, 1.162755e-11)
                with self.assertRaises(AttributeError):
                    secular.rates_per_year[0].c = 2e-11
                self.assertEqual(catalog.gravity_models[0].info.secular_variations.reference_epoch.tai_nanoseconds,
                                 -32_184_000_000)
                data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", receipt.file)
                project = of.Project("Secular", of.Epoch(0), of.Epoch(600 * 10**9))
                sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                    initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
                    gravity_model="custom", gravity_degree=8, gravity_order=4,
                    include_secular_variations=True))
                analysis = project.prepare(data=data)
                before = analysis.states(sat, step=u.s(60))
                self.assertIn("secular=file-linear-julian-year-v1", before.provenance.algorithm)
                path = root / "project.orbitforge"
                project.save(path, analysis=analysis)
                restored = of.Project.open(path, data=data)
                self.assertTrue(restored.objects[sat.id].orbit.hpop.include_secular_variations)
                changed_file = root / "shifted.grv"
                changed_file.write_text(self.SOURCE.read_text().replace("2451545.0", "2453371.5"))
                changed = data.with_gravity("custom", changed_file)
                plan = of.Project.plan_open(path, data=changed)
                self.assertEqual([v.role for v in plan.data_changes], ["gravity/custom"])
                with self.assertRaises(of.DataChangedError):
                    plan.open()
                accepted = plan.open(accept_data_changes=True)
                after = accepted.prepare(data=changed).states(accepted.objects[sat.id], step=u.s(60))
                self.assertNotEqual(after.position.values[-1], before.position.values[-1])
                self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
                self.assertNotEqual(after.data_sha256, before.data_sha256)
            finally:
                del catalog
            reopened = of.DataCatalog(root / "catalog")
            try:
                self.assertEqual(reopened.gravity_models[0].info.secular_variations.reference_epoch.tai_nanoseconds,
                                 -32_184_000_000)
            finally:
                del reopened

    def test_corrupt_secular_receipts_are_rejected_without_changing_raw_files(self):
        with tempfile.TemporaryDirectory() as folder:
            catalog = of.DataCatalog(Path(folder) / "catalog")
            try:
                receipt = catalog.install_gravity(self.SOURCE)
                saved = catalog.setting("gravity/custom")
                raw = Path(receipt.file).read_bytes()
                bad_values = [
                    {"epoch_tai_ns": 0.25}, {"epoch_tai_ns": 2**63},
                    {"epoch_tai_ns": -2**63 - 1}, {"rates_per_julian_year": []},
                    {"rates_per_julian_year": [[2, 0, 0, 1e-11]]},
                    {"rates_per_julian_year": [[2, 0, 1e-11, 0], [2, 0, 2e-11, 0]]},
                    {"rates_per_julian_year": [[3, 0, 1e-11, 0], [2, 0, 2e-11, 0]]},
                    {"rates_per_julian_year": [[2191, 0, 1e-11, 0]]},
                    {"rates_per_julian_year": [[8, 5, 1e-11, 0]]},
                    {"rates_per_julian_year": [[2.5, 0, 1e-11, 0]]},
                    {"rates_per_julian_year": [[2, 0, "nan", 0]]},
                ]
                for value in bad_values:
                    with self.subTest(value=value):
                        broken = json.loads(saved)
                        broken["secular_variations"].update(value)
                        catalog.set_setting("gravity/custom", json.dumps(broken))
                        with self.assertRaises((RuntimeError, ValueError)):
                            _ = catalog.gravity_models
                        self.assertEqual(Path(receipt.file).read_bytes(), raw)
                old = json.loads(saved)
                old["schema"] = 2
                catalog.set_setting("gravity/custom", json.dumps(old))
                with self.assertRaisesRegex(RuntimeError, "reimport"):
                    _ = catalog.gravity_models
                catalog.set_setting("gravity/custom", saved)
                self.assertEqual(catalog.gravity_models[0].info.secular_variations.reference_epoch.tai_nanoseconds,
                                 -32_184_000_000)
            finally:
                del catalog

    def test_static_file_cannot_enable_variations_and_flag_requires_bool(self):
        with self.assertRaisesRegex(TypeError, "include_secular_variations"):
            of.orbits.HpopSettings(include_secular_variations=1)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            file = root / "static.grv"
            file.write_text(GRV)
            data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", file)
            project = of.Project("Static", of.Epoch(0), of.Epoch(600 * 10**9))
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
                gravity_model="custom", gravity_degree=2, include_secular_variations=True))
            with self.assertRaisesRegex(ValueError, "secular reference epoch"):
                project.prepare(data=data)
            sat.orbit = replace(sat.orbit, hpop=replace(sat.orbit.hpop, include_secular_variations=False))
            self.assertIsNotNone(project.prepare(data=data))

    def test_point_mass_preserves_inactive_flag_without_changing_analysis(self):
        project = of.Project("Draft", of.Epoch(0), of.Epoch(600 * 10**9))
        sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
            initial=of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300)),
            gravity_degree=0, include_secular_variations=False))
        before = project.prepare().states(sat, step=u.s(60))
        sat.orbit = replace(sat.orbit, hpop=replace(sat.orbit.hpop, include_secular_variations=True))
        after = project.prepare().states(sat, step=u.s(60))
        self.assertEqual(before.input_sha256, after.input_sha256)
        self.assertEqual(before.position.values, after.position.values)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "draft.orbitforge"
            project.save(path)
            self.assertTrue(of.Project.open(path).objects[sat.id].orbit.hpop.include_secular_variations)
