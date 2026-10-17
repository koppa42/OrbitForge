# Historical solar-system SPICE sample

OrbitForge-produced DE440s validation subset, copied without refitting by CSPICE N0067 `spksub`.
Retained targets: 1..10 relative to SSB 0, Mercury 199 relative to 1, Venus 299 relative to 2,
and Earth 399 / Moon 301 relative to EMB 3. All segments retain their original J2000 axes and Type 2 coefficients.
The binary and manifest have the same historical intervals as the [Earth/Moon/Sun sample](../de440s_2016_leap/README.md).
The unmodified NAIF LSK, original source hashes, source URLs and derived producer are recorded in `manifest.json`.
This is a validation input, not a current-epoch ephemeris.

Recreate from the pinned full DE440s and naif0012.tls with `orbitforge_spice_extract SOURCE LSK NEW-output.bsp --all-planets`.
The source file is not stored in the repository; only the small subset is redistributed.
See the [NAIF redistribution and modification rules](https://naif.jpl.nasa.gov/naif/rules.html).

Independent comparison uses `tools/validation/compare_spice_ephemeris.py --all-planets` with the full original DE440s,
jplephem 2.24 and NumPy 2.2.6, including the endpoints and leap-second vicinity. The original source hash is pinned; subset extracted 2026-10-14.
