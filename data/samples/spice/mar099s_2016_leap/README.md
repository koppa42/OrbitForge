# MAR099s planetary-centre validation input

OrbitForge-produced historical subset of [NAIF MAR099s](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/satellites/mar099s.bsp), copied with CSPICE N0067 `spksub` without refitting. The original file is 67,594,240 bytes, SHA-256 `997dc93ba640e476da7a494d2237dcdeb145e528db37be8ccee588c615e4e1ff`.

`mar099s_subset.bsp` is 38,912 bytes, SHA-256 `8f00ea59698cbdff7132dba633a73e7c00218bc0373cc672edccbb895e594769`. It retains the original seven Type 2 J2000 targets 401, 402, 499, 399, 10, 4 and 3 from 2016-12-27 to 2017-01-05 TDB. The embedded comments identify OrbitForge as the derivative producer. Extra Sun/Earth/barycentre segments deliberately exercise import filtering; this is not a ready-to-load supplemental kernel.

`mars_jplephem.txt` contains 37 independent reference states for Mars relative to its system barycentre and Earth, in metres and metres per TDB second. JPLEPHEM reads the full original Mars kernel, combined with the pinned DE440s planets sample (SHA-256 `e4c52121a5de093ea75f9826d5db48aeba25bb0f8bbe542e6fa07c6a0dd824fd`). Epochs use int64 TAI nanoseconds and the matching TDB seconds. Integral/fractional Julian dates avoid cancellation in Chebyshev evaluation.

Recreate in a new output folder with JPLEPHEM 2.24, NumPy 2.2.6, SpiceyPy 7.0.0 / CSPICE N0067:

```sh
python tools/validation/generate_planet_centres_reference.py --source /path/to/mar099s.bsp --output-dir /path/to/new-reference
```

The generator verifies source hashes and dependency versions and refuses to overwrite an existing binary. See the [NAIF redistribution and modification rules](https://naif.jpl.nasa.gov/naif/rules.html) and [validation protocol](../../../../docs/validation/p0_spice_protocol.md). This small offline fixture is not current-epoch ephemeris data.
