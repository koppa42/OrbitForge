"""Compare installed OrbitForge states with independent jplephem reads of the full DE440s source."""
import argparse
import gc
import hashlib
import importlib.metadata
import json
from pathlib import Path
from random import Random

import numpy as np
from jplephem.spk import SPK
import orbitforge as of

POSITION_TOLERANCE_M = 1e-3
VELOCITY_TOLERANCE_MPS = 1e-7


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--reference-spk", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--all-planets", action="store_true", help="Include all HPOP planetary targets")
    args = parser.parse_args()
    if importlib.metadata.version("jplephem") != "2.24" or np.__version__ != "2.2.6":
        raise ValueError("Reference environment must use jplephem 2.24 and NumPy 2.2.6")
    manifest = json.loads(args.manifest.read_text())
    source_hash = hashlib.sha256(args.reference_spk.read_bytes()).hexdigest()
    if source_hash != "c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2":
        raise ValueError("Full reference DE440s source hash mismatch")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    provider = of.load_spice_snapshot(args.manifest, args.work_dir / "kernels")
    first, last = provider.data_info.first_epoch.tai_nanoseconds, provider.data_info.last_epoch.tai_nanoseconds
    ticks = {first+(last-first)*i//64 for i in range(65)}
    random = Random(4402016)
    ticks.update(random.randrange(first, last+1) for _ in range(64))
    midnight = of.make_builtin_time_provider().from_utc(of.UtcDateTime(2017, 1, 1)).tai_nanoseconds
    ticks.update(midnight + offset for offset in (-2_000_000_000, -1_000_000_000, -500_000_000, -1, 0, 1, 500_000_000, 1_000_000_000))
    pairs = [(10, 399), (301, 399), (399, 0), (3, 0), (10, 301), (399, 301)]
    if args.all_planets:
        pairs += [(target, 399) for target in (199, 299, 4, 5, 6, 7, 8, 9)]
    max_position = max_velocity = max_time_roundtrip = 0.0
    rows = []
    with SPK.open(str(args.reference_spk)) as reference:
        def barycentric(body, et):
            days, seconds = divmod(et, 86400.0)
            jd1, jd2 = 2451545.0 + days, seconds / 86400.0
            def segment(center, target):
                p, v_per_day = reference[center, target].compute_and_differentiate(jd1, jd2)
                return np.concatenate((p*1000, v_per_day*(1000/86400)))
            if body == 0:
                return np.zeros(6)
            if 1 <= body <= 10:
                return segment(0, body)
            if body in (199, 299):
                return segment(0, body // 100) + segment(body // 100, body)
            return segment(0, 3) + segment(3, body)
        for ns in sorted(ticks):
            epoch = of.Epoch(ns)
            tdb = provider.to_tdb(epoch)
            max_time_roundtrip = max(max_time_roundtrip, abs(provider.from_tdb(tdb).seconds_since(epoch)))
            for target, observer in pairs:
                actual = provider.geometric_state(target, observer, epoch)
                expected = barycentric(target, tdb.seconds_past_j2000) - barycentric(observer, tdb.seconds_past_j2000)
                values = np.array(list(actual.position_m) + list(actual.velocity_mps))
                if not np.isfinite(values).all() or not np.isfinite(expected).all():
                    raise ValueError("Nonfinite state in comparison")
                error = np.abs(values-expected)
                max_position = max(max_position, float(error[:3].max()))
                max_velocity = max(max_velocity, float(error[3:].max()))
                rows.append([ns, tdb.seconds_past_j2000, target, observer, *expected.tolist()])
    report = {"reference":"jplephem 2.24 / NumPy 2.2.6, full original DE440s",
              "epochs":len(ticks), "state_pairs":len(pairs), "states":len(rows),
              "source_sha256":source_hash, "manifest_sha256":provider.data_info.manifest_sha256,
              "kernels":manifest["kernels"], "position_tolerance_m":POSITION_TOLERANCE_M,
              "velocity_tolerance_mps":VELOCITY_TOLERANCE_MPS,
              "max_position_component_error_m":max_position,
              "max_velocity_component_error_mps":max_velocity,
              "max_tai_tdb_roundtrip_error_s":max_time_roundtrip}
    (args.work_dir / "report.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    (args.work_dir / "reference_states.json").write_text(json.dumps(rows)+"\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    del provider
    gc.collect()
    if max_position > POSITION_TOLERANCE_M or max_velocity > VELOCITY_TOLERANCE_MPS:
        raise RuntimeError("Independent SPK comparison failed")
    if max_time_roundtrip > 1e-6:
        raise RuntimeError("Time roundtrip exceeds the validation-window tolerance")


if __name__ == "__main__":
    main()
