"""Generate a fixed Mars SPK subset and independent JPLEPHEM reference.

Pinned: JPLEPHEM 2.24, NumPy 2.2.6, SpiceyPy 7.0.0 / CSPICE N0067.
SPKSUB copies original coefficients; JPLEPHEM evaluates the original full kernel,
not the derived subset or an OrbitForge library. Time conversion uses NAIF unitim.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import jplephem
from jplephem.spk import SPK
import numpy as np
import spiceypy as spice

MARS_SHA = "997dc93ba640e476da7a494d2237dcdeb145e528db37be8ccee588c615e4e1ff"
BASE_SHA = "e4c52121a5de093ea75f9826d5db48aeba25bb0f8bbe542e6fa07c6a0dd824fd"
MARS_URL = "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/satellites/mar099s.bsp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    assert (jplephem.__version__, np.__version__, spice.__version__) == ("2.24", "2.2.6", "7.0.0")
    assert spice.tkvrsn("TOOLKIT") == "CSPICE_N0067"
    root = Path(__file__).resolve().parents[2]
    base_dir = root / "data/samples/spice/de440s_planets_2016_leap"
    metadata = json.loads((base_dir / "manifest.json").read_text(encoding="utf-8"))
    base = base_dir / metadata["kernels"][1]["file"]
    for path, expected in ((args.source, MARS_SHA), (base, BASE_SHA)):
        with path.open("rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == expected
    args.output_dir.mkdir(parents=True, exist_ok=True)
    target = args.output_dir / "mar099s_subset.bsp"
    if target.exists():
        raise RuntimeError("Refusing to replace an existing SPK")
    spice.furnsh(str(base_dir / "naif0012.tls"))
    start, end = [spice.str2et(value) for value in ("2016 DEC 27 00:00:00 TDB", "2017 JAN 05 00:00:00 TDB")]
    source = spice.dafopr(str(args.source.resolve()))
    dest = spice.spkopn(str(target.resolve()), "OrbitForge MAR099s validation subset", 2048)
    spice.dafac(dest, [
        "Producer of this derived kernel: OrbitForge project.",
        "CSPICE N0067 SPKSUB; original coefficients were not refitted.",
        "Source: NAIF mar099s.bsp; SHA256 " + MARS_SHA,
        "Retained original targets 401,402,499,399,10,4,3 and J2000 axes.",
        "2016 DEC 27 to 2017 JAN 05 TDB. Offline validation only.",
    ])
    spice.dafbfs(source)
    targets = []
    while spice.daffna():
        descriptor = spice.dafgs(5)
        bounds, ids = spice.dafus(descriptor, 2, 6)
        assert ids[2] == 1 and ids[3] == 2 and bounds[0] <= start and bounds[1] >= end
        spice.spksub(source, descriptor, "OrbitForge MAR099s subset " + str(ids[0]), start, end, dest)
        targets.append(int(ids[0]))
        spice.dafcs(source)
    assert set(targets) == {401,402,499,399,10,4,3}
    spice.spkcls(dest)
    spice.dafcls(source)
    epochs = {metadata["first_tai_ns"] + i * (metadata["last_tai_ns"] - metadata["first_tai_ns"]) // 32 for i in range(33)}
    epochs.update((536500835999999999,536500836000000000,536500836999999999,536500837000000000))
    rows = ["# JPLEPHEM 2.24 / NumPy 2.2.6; original MAR099s + pinned DE440s base; SI, J2000, TDB velocity",
            "# tai_ns et_tdb_s; Mars/4 position(3) velocity(3); Mars/Earth position(3) velocity(3)"]
    with SPK.open(str(args.source)) as mars, SPK.open(str(base)) as de:
        for ns in sorted(epochs):
            seconds = ns // 1000000000 + (ns % 1000000000) / 1e9
            et = spice.unitim(seconds,"TAI","TDB")
            days = math.floor(et/86400.)
            jd1, jd2 = 2451545. + days, (et-days*86400.)/86400.
            local_p, local_v = mars[4,499].compute_and_differentiate(jd1,jd2)
            p4,v4 = de[0,4].compute_and_differentiate(jd1,jd2)
            p3,v3 = de[0,3].compute_and_differentiate(jd1,jd2)
            pe,ve = de[3,399].compute_and_differentiate(jd1,jd2)
            values = (et,* (local_p*1000),* (local_v*(1000/86400)),
                      * ((p4-p3-pe+local_p)*1000),* ((v4-v3-ve+local_v)*(1000/86400)))
            rows.append(str(ns)+" "+" ".join(format(float(value),".17g") for value in values))
    (args.output_dir/"mars_jplephem.txt").write_text("\n".join(rows)+"\n",encoding="utf-8",newline="\n")
    spice.kclear()
    print(json.dumps({"cases":len(rows)-2,"source_sha256":MARS_SHA,"subset_sha256":hashlib.sha256(target.read_bytes()).hexdigest(),"subset_bytes":target.stat().st_size}))


if __name__ == "__main__":
    main()
