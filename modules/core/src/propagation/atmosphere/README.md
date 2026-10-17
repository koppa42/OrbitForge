# Atmospheric scalar kernels

Both compiled C++ kernels use only the standard library and local scratch state.
Public inputs/validation, WGS84 geometry, low-altitude blending and the spherical
drag force are in `atmospheric_drag.cpp`; resources are injected by HPOP/Runtime.

`jacchia_roberts.cpp` adapts the scalar methods/tables from NASA GMAT commit
[`c9b78bd1c8499845cb78fde315e1f761b6b06f1c`](https://github.com/nasa/GMAT/blob/c9b78bd1c8499845cb78fde315e1f761b6b06f1c/src/base/solarsys/JacchiaRobertsAtmosphere.cpp).
It retains GMAT's fitted constants and WGS84 polar radius; it removes GMAT's
object/file integration, uses finite noon/midnight/pole/equinox limits, and bounds
polynomial-root iteration. High-altitude density temperature ratios are evaluated
from the original exponential directly, avoiding subtraction of rounded near-equal
temperatures. The original equations are independently checked at 80/100 digits. The scalar 90–100 km regime is exposed; GMAT's outer
`Density` method disables that regime, so the independent reference harness
calls its original scalar `JacchiaRoberts` method directly.

`nrlmsise00.cpp` transcribes the double precision `Output` methods/tables from
[Orekit 13.1.2](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/models/earth/atmosphere/NRLMSISE00.java).
It calls the gtd7d path including anomalous oxygen, with default variations and
daily Ap (`SW[9]=1`) or explicit seven-value Ap history (`SW[9]=-1`).
Fixed arrays replace Java allocations; spline scratch is bounded to
five nodes. Inputs retain native F10.7 units (sfu) and convert manual Kp to Ap
using the standard 1/3-Kp table and linear interpolation.

The independently compiled author C code and unmodified NASA GMAT scalar model
are used only by reference producers. They never link these production kernels.
Pinned sources, reproduction, scopes and tolerances are recorded in the density
validation protocol. License and attribution are installed with SDK/Python.
