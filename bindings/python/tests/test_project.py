from pathlib import Path
import hashlib
import json
import struct
import tempfile
import unittest
# Native value/format tests; public editing is in test_objects.py.
from orbitforge import _native as of
from project_format import rewrite_section


class ProjectTests(unittest.TestCase):
    def test_settings_keep_two_body_and_j2_initial_conditions_and_roundtrip(self):
        original = self.make_project()
        first, last = of.Epoch(700123456790), of.Epoch(1800123456789)
        revised = original.with_settings('修改后的任务', first, last)
        self.assertEqual(revised.current.tai_nanoseconds, first.tai_nanoseconds)
        before = original.to_scenario().sample([first, last])
        after = revised.to_scenario().sample([first, last])
        for old, new, old_states, new_states in zip(original.satellites, revised.satellites,
                                                  before, after):
            self.assertEqual(new.orbit.initial.epoch.tai_nanoseconds,
                             old.orbit.initial.epoch.tai_nanoseconds)
            self.assertEqual(new.orbit.first.tai_nanoseconds, first.tai_nanoseconds)
            self.assertEqual(new.orbit.last.tai_nanoseconds, last.tai_nanoseconds)
            self.assertEqual(new.orbit.object_id, old.orbit.object_id)
            for x, y in zip(old_states.states, new_states.states):
                self.assertEqual(x.position_m, y.position_m)
                self.assertEqual(x.velocity_mps, y.velocity_mps)
        scenario = original.to_scenario()
        changed = scenario.with_settings(revised.name, first, last)
        self.assertEqual(changed.revision, scenario.revision + 1)
        self.assertEqual(changed.object_count, scenario.object_count)
        self.assertEqual(scenario.name, original.name)
        with self.assertRaises(IndexError):
            changed.sample([original.first])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '修改时段.orbitforge'
            revised.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.name, revised.name)
            self.assertEqual(restored.first.tai_nanoseconds, first.tai_nanoseconds)
            self.assertEqual(restored.current.tai_nanoseconds, first.tai_nanoseconds)
        self.assertEqual(original.name, '任务 Python')
        self.assertEqual(original.first.tai_nanoseconds, 123456789)

    def test_settings_validate_existing_coverage_and_invalid_intervals(self):
        original = self.bound_project()
        beyond = of.Epoch(original.earth_orientation.last_epoch.tai_nanoseconds + 1)
        with self.assertRaises(of.ProjectError):
            original.with_settings('Out of coverage', original.first, beyond)
        with self.assertRaises(of.ProjectError):
            original.with_settings('Reversed', original.last, original.first)
        with self.assertRaises(of.ProjectError):
            original.with_settings(' ', original.first, original.last)
        earlier = of.Epoch(original.current.tai_nanoseconds - 1)
        changed = original.with_settings('Clamped', original.first, earlier)
        self.assertEqual(changed.current.tai_nanoseconds, earlier.tai_nanoseconds)
        self.assertEqual(changed.data_lock.sha256, original.data_lock.sha256)

    def bound_project(self):
        project = self.make_project()
        manifest = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json'
        project.earth_orientation = of.load_eop_snapshot(manifest).data_info
        return project

    def test_data_lock_has_independent_sha256_and_is_preserved_in_runtime_scenarios(self):
        project = self.bound_project()
        lock = project.data_lock
        self.assertEqual(lock.sha256, hashlib.sha256(lock.canonical_bytes()).hexdigest())
        self.assertEqual([r.role for r in lock.references], ['earth-orientation', 'utc-time'])
        self.assertEqual(lock.references[0].content_sha256, project.earth_orientation.sha256)
        self.assertEqual(lock.references[1].content_sha256, project.earth_orientation.time_data.sha256)
        self.assertEqual(of.DataLock(list(reversed(lock.references))).sha256, lock.sha256)
        original = project.to_scenario()
        self.assertEqual(original.data_lock.sha256, lock.sha256)
        changed = original.with_data_lock(of.DataLock())
        self.assertEqual(changed.revision, original.revision + 1)
        self.assertEqual(original.data_lock.sha256, lock.sha256)
        self.assertEqual(project.renamed_satellite('二体', 'New name').data_lock.sha256, lock.sha256)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bound.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.data_lock.sha256, lock.sha256)
            self.assertEqual(restored.to_scenario().data_lock.sha256, lock.sha256)

    def test_content_hash_matches_the_actual_pinned_file(self):
        directory = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02'
        manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        raw = (directory / manifest['data_file']).read_bytes()
        self.assertEqual(self.bound_project().data_lock.references[0].content_sha256,
                         hashlib.sha256(raw).hexdigest())

    def test_missing_inconsistent_or_wrong_digest_locks_fail_even_with_valid_crcs(self):
        project = self.bound_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bound.orbitforge'
            project.save(path)
            original = path.read_bytes()
            self.assertEqual(struct.unpack_from('<I', original, 8)[0], 40)
            def rewrite(tag_to_change, mutate):
                path.write_bytes(rewrite_section(original, tag_to_change, mutate))

            rewrite(b'LOCK', lambda body: None)
            with self.assertRaisesRegex(of.ProjectError, 'Missing scenario data lock'):
                of.load_project(path)
            rewrite(b'LOCK', lambda body: body[:-1] + (b'0' if body[-1:] != b'0' else b'1'))
            with self.assertRaisesRegex(of.ProjectError, 'SHA256 mismatch'):
                of.load_project(path)
            digest = project.earth_orientation.sha256.encode('ascii')
            changed = (b'0' if digest[:1] != b'0' else b'1') + digest[1:]
            rewrite(b'DATA', lambda body: body.replace(digest, changed, 1))
            with self.assertRaisesRegex(of.ProjectError, 'references disagree'):
                of.load_project(path)

    def test_extrapolation_policy_roundtrip_and_integrity(self):
        project = self.bound_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.orbitforge'
            project.save(path)
            loaded = of.load_project(path)
            sample = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json'
            loaded.earth_orientation = of.load_eop_snapshot(sample, 3).data_info
            loaded.save(path, overwrite=True)
            restored = of.load_project(path)
            self.assertEqual(restored.earth_orientation.extrapolation_days, 3)
            self.assertEqual(restored.data_lock.sha256, loaded.data_lock.sha256)
            self.assertNotEqual(restored.data_lock.sha256, project.data_lock.sha256)
            self.assertEqual(restored.earth_orientation.sha256, project.earth_orientation.sha256)
            original = path.read_bytes()
            # A changed policy with repaired CRCs must still fail the independent data hash.
            path.write_bytes(rewrite_section(original, b'DATA', lambda body: body[:-4] + struct.pack('<I', 4)))
            with self.assertRaisesRegex(of.ProjectError, 'references disagree'):
                of.load_project(path)

    def test_names_copies_and_deletions_preserve_stable_ids_and_old_values(self):
        original = self.make_project()
        renamed = original.renamed_satellite('二体', '观测卫星')
        self.assertEqual(original.satellites[0].name, '二体')
        self.assertEqual(renamed.satellites[0].orbit.object_id, '二体')
        copied = renamed.with_satellite_copy('二体', 'copy-1', '观测卫星 副本')
        self.assertEqual(len(copied.satellites), 3)
        states = copied.to_scenario().sample([copied.first, copied.last])
        self.assertEqual(states[0].states[1].position_m, states[2].states[1].position_m)
        reduced = copied.without_satellite('二体')
        self.assertEqual([s.orbit.object_id for s in reduced.satellites], ['J2', 'copy-1'])
        self.assertEqual(copied.to_scenario().without_object('二体').object_count, 2)
        with self.assertRaises(of.ProjectError):
            copied.with_satellite_copy('copy-1', 'J2', 'duplicate ID')
        with self.assertRaises(of.ProjectError):
            copied.renamed_satellite('copy-1', ' ')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'names.orbitforge'
            copied.save(path)
            self.assertEqual(of.load_project(path).satellites[0].name, '观测卫星')

    def make_project(self):
        project = of.Project('任务 Python', of.Epoch(123456789), of.Epoch(7200123456789))
        project.current = of.Epoch(600123456789)
        for name, kind in [('二体', of.OrbitPropagator.TWO_BODY), ('J2', of.OrbitPropagator.J2_SECULAR)]:
            orbit = of.OrbitInput()
            orbit.object_id, orbit.propagator = name, kind
            orbit.input_frame = of.FrameId.ITRF
            project.add_satellite(orbit)
        return project

    def test_file_round_trip_preserves_inputs_and_computed_states(self):
        project = self.make_project()
        before = project.to_scenario().sample([project.first, project.current, project.last])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '任务.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.name, project.name)
            self.assertEqual(restored.current.tai_nanoseconds, project.current.tai_nanoseconds)
            self.assertEqual(restored.satellites[1].orbit.propagator, of.OrbitPropagator.J2_SECULAR)
            self.assertEqual(restored.satellites[1].orbit.input_frame, of.FrameId.ITRF)
            after = restored.to_scenario().sample([project.first, project.current, project.last])
            for a, b in zip(before, after):
                self.assertEqual(a.provenance.algorithm, b.provenance.algorithm)
                for x, y in zip(a.states, b.states):
                    self.assertEqual(x.position_m, y.position_m)
                    self.assertEqual(x.velocity_mps, y.velocity_mps)
            second = Path(directory) / 'roundtrip.orbitforge'
            restored.save(second)
            self.assertEqual(path.read_bytes(), second.read_bytes())

    def test_save_failures_leave_original_and_never_accept_corruption(self):
        project = self.make_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'project.orbitforge'
            project.save(path)
            original = path.read_bytes()
            project.name = 'Changed'
            with self.assertRaises(of.ProjectError):
                project.save(path)
            self.assertEqual(path.read_bytes(), original)
            project.save(path, overwrite=True)
            self.assertEqual(of.load_project(path).name, 'Changed')
            project.current = of.Epoch(-1)
            with self.assertRaises(of.ProjectError):
                project.save(path, overwrite=True)
            self.assertEqual(of.load_project(path).name, 'Changed')
            bad = bytearray(original)
            bad[-1] ^= 1
            path.write_bytes(bad)
            with self.assertRaises(of.ProjectError):
                of.load_project(path)

    def test_duplicate_insert_is_transactional_and_empty_project_is_valid(self):
        project = self.make_project()
        with self.assertRaises(of.ProjectError):
            project.add_satellite(project.satellites[0].orbit)
        self.assertEqual(len(project.satellites), 2)
        empty = of.Project('Empty', of.Epoch(0), of.Epoch(1))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'empty.orbitforge'
            empty.save(path)
            self.assertEqual(of.load_project(path).to_scenario().object_count, 0)
