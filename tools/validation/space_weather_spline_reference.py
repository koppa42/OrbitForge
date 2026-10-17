"""Independent SciPy 1.16.3 natural cubic reference; no OrbitForge imports.

Run: python tools/validation/space_weather_spline_reference.py OUTPUT [GMAT_DRIVER]
The two series are independent (Kp is not derived from Ap). Civil UTC slots
have unit spacing; evaluation fractions are elapsed TAI between midpoint knots.
Inputs span the 2016 positive leap second, both sides of knots, and run endpoints.
"""
import datetime as dt
import math
from pathlib import Path
import sys
import subprocess
import numpy as np
import scipy
from scipy.interpolate import CubicSpline

assert scipy.__version__ == "1.16.3", scipy.__version__
start = dt.datetime(2016, 12, 24, 1, 30)
origin = dt.datetime(2000, 1, 1, 12)
count = 112

def tai_ns(utc):
    offset = 36 if utc < dt.datetime(2017, 1, 1) else 37
    span = utc - origin
    return (span.days * 86400 + span.seconds + offset) * 1_000_000_000

nodes = [tai_ns(start + dt.timedelta(hours=3*i)) for i in range(count)]
ap = [35 + 12*math.sin(i*.28) + 5*math.cos(i*.63) + i*.15 for i in range(count)]
kp = [3 + .7*math.cos(i*.21) + .4*math.sin(i*.67) for i in range(count)]
lines = ["# SciPy 1.16.3 CubicSpline(bc_type='natural', extrapolate=False)",
         "# D: day_index slot_index Ap Kp; first date 2016-12-24; flux/center81=150",
         "# C: source(0=Ap,1=Kp) query_TAI_ns selected_index; JR includes 24120 s lag"]
for i in range(count):
    lines.append(f"D {i//8} {i%8} {ap[i]:.17g} {kp[i]:.17g}")
for source, values in enumerate((ap, kp)):
    spline = CubicSpline(np.arange(count), values, bc_type='natural', extrapolate=False)
    for i in range(count-1):
        for numerator in (0, 1, 123456789, (nodes[i+1]-nodes[i])//2, nodes[i+1]-nodes[i]-1):
            fraction = numerator / (nodes[i+1]-nodes[i])
            at = nodes[i] + numerator + 24120_000_000_000
            lines.append(f"C {source} {at} {float(spline(i+fraction)):.17g}")
    lines.append(f"C {source} {nodes[-1] + 24120_000_000_000} {values[-1]:.17g}")
if len(sys.argv) == 3:
    # Convert the selected geomagnetic quantity before passing it to original GMAT.
    ap_table = [0,2,3,4,5,6,7,9,12,15,18,22,27,32,39,48,56,67,80,94,111,132,154,179,207,236,300,400]
    records = [row.split() for row in lines if row.startswith('C ')][::17]
    inputs, metadata = [], []
    for j, row in enumerate(records):
        source, ns, index = int(row[1]), int(row[2]), float(row[3])
        kp_value = float(np.interp(index, ap_table, np.arange(28)/3)) if source == 0 else index
        offset = 36 if ns < tai_ns(dt.datetime(2017,1,1)) else 37
        utc = origin + dt.timedelta(seconds=ns//1_000_000_000-offset)
        seconds = utc.hour*3600 + utc.minute*60 + utc.second + (ns % 1_000_000_000)*1e-9
        # Explicit positive leap-second mapping, independent of a time library.
        leap_begin = tai_ns(dt.datetime(2017,1,1))-1_000_000_000
        if leap_begin <= ns < leap_begin+1_000_000_000:
            utc = dt.datetime(2016,12,31); seconds = 86400 + (ns-leap_begin)*1e-9
        mjd = 21544.5 + (utc.date()-dt.date(2000,1,1)).days + seconds/86400
        height = (100_000,126_000,400_000,1_000_000)[j % 4]
        latitude = (-.7,.2,.8)[j % 3]; angle=1.2; declination=.2
        inputs.append(' '.join(format(v,'.17g') for v in
                      (mjd,height,latitude,angle,declination,150,150,kp_value)))
        metadata.append((source,ns,utc.year,utc.month,utc.day,seconds,height,latitude,angle,declination))
    result = subprocess.run([sys.argv[2]], input='\n'.join(inputs)+'\n',
                            text=True, encoding='ascii', stdout=subprocess.PIPE, check=True)
    densities = result.stdout.splitlines()
    assert len(densities) == len(metadata)
    lines.append('# R: source TAI_ns year month day UTC_seconds height_m latitude_rad hour_angle_rad sun_declination_rad GMAT_density_kg_m3')
    for meta, rho in zip(metadata,densities):
        assert float(rho) > 0
        lines.append('R '+' '.join(str(v) if isinstance(v,int) else format(v,'.17g') for v in meta)+' '+rho)
Path(sys.argv[1]).write_text("\n".join(lines)+"\n", encoding='ascii', newline='\n')
