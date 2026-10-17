from dataclasses import replace
from pathlib import Path
import math
import tempfile
import unittest
import orbitforge as of

REFERENCE = Path(__file__).resolve().parents[3] / "tests/reference/propagators/vop_interpolation.txt"
BASE = 8_000_000_000_000_000_123
MU = 3.986004415e14


class VopInterpolationTests(unittest.TestCase):
    def test_installed_api_matches_orekit_hipparchus_all_orders_and_mu(self):
        count = 0
        for line in REFERENCE.read_text(encoding="ascii").splitlines():
            if line.startswith("#"):
                continue
            row = line.split()
            if row[0] == "C":
                _, kind, order, step, mu, size = row
                config = of.OrbitInterpolationOptions()
                config.method = of.OrbitInterpolationMethod.VOP
                config.order = int(order)
                config.vop_mu = float(mu)
                nodes, session = [], None
            else:
                epoch = of.Epoch(BASE + int(row[1]))
                y = list(map(float, row[2:]))
                if row[0] == "N":
                    nodes.append(of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.GCRF, y[:3], y[3:6]))
                else:
                    if session is None:
                        self.assertEqual(len(nodes), int(size))
                        session = of.make_tabulated_orbit_session([nodes], config)
                        self.assertIn("vop-universal-transport-v1", session.provenance.algorithm)
                        self.assertIn("vop-mu=", session.provenance.algorithm)
                    actual = session.sample([epoch]).states[0]
                    with self.subTest(kind=kind, order=order, ns=row[1]):
                        self.assertLess(math.dist(actual.position_m, y[:3]), 2e-4)
                        self.assertLess(math.dist(actual.velocity_mps, y[3:6]), 2e-7)
                        self.assertEqual(actual.epoch.tai_nanoseconds, epoch.tai_nanoseconds)
                    count += 1
        self.assertEqual(count, 1059)

    def test_independent_mu_roundtrip_effective_hash_and_immutable_analysis(self):
        initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                    [7e6, 0, 0], [0, math.sqrt(MU/7e6), 0])
        project = of.Project("VOP", of.Epoch(0), of.Epoch(1200*10**9))
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0, interpolation="vop",
                              interpolation_order=1, interpolation_mu=MU,
                              report_ephemeris_on_fixed_step=True, max_step_seconds=300)
        satellite = project.objects.satellite("VOP", orbit=replace(orbit, step_seconds=60))
        epoch = of.Epoch(317_123_456_789)
        analysis = project.prepare()
        before = analysis.states(satellite, times=[epoch])
        theta = math.sqrt(MU/7e6**3)*(epoch.tai_nanoseconds/1e9)
        expected = [7e6*math.cos(theta), 7e6*math.sin(theta), 0]
        self.assertLess(math.dist(analysis.state(satellite, at=epoch).position_m, expected), .001)
        self.assertEqual(satellite.orbit.hpop._native().interpolation.vop_mu, MU)
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, interpolation_mu=MU*1.01))
        changed = project.prepare().states(satellite, times=[epoch])
        self.assertNotEqual(changed.input_sha256, before.input_sha256)
        self.assertNotEqual(changed.position.values, before.position.values)
        self.assertEqual(analysis.states(satellite, times=[epoch]).position.values, before.position.values)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"vop.orbitforge"
            project.save(path)
            reopened = of.Project.open(path)
            restored = reopened.objects[satellite.id]
            self.assertEqual(restored.orbit.hpop, satellite.orbit.hpop)
            self.assertEqual(reopened.prepare().states(restored, times=[epoch]).position.values, changed.position.values)
        # Inactive VOP GM is preserved but cannot alter effective calculations or identity.
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, interpolation="hermite", interpolation_order=7))
        hermite = project.prepare().states(satellite, times=[epoch])
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, interpolation_mu=MU*1.02))
        inactive = project.prepare().states(satellite, times=[epoch])
        self.assertEqual(hermite.input_sha256, inactive.input_sha256)
        self.assertEqual(hermite.position.values, inactive.position.values)
        revision = project.revision
        for bad in (0, -1, float("nan"), float("inf")):
            with self.subTest(mu=bad), self.assertRaises(ValueError):
                satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, interpolation_mu=bad))
        self.assertEqual(project.revision, revision)
