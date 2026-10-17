"""Independent SciPy 1.16.3 primitive-spline / GMAT reference, no OrbitForge imports.

python tools/validation/space_weather_mean_reference.py OUTPUT GMAT_DRIVER
The derivative of a natural cubic through cumulative bin integrals is quadratic.
Scale its deviation from each mean using the analytic extrema, preserving the
integral and Ap/Kp bounds. UTC bins have unit spacing; elapsed TAI (including the
2016 leap second) supplies the within-bin coordinate. Missing fields split runs.
"""
import datetime as dt
import math
from pathlib import Path
import subprocess
import sys
import numpy as np
import scipy
from scipy.interpolate import CubicSpline
from numpy.polynomial import Polynomial
from numpy.polynomial.legendre import leggauss

assert np.__version__ == "2.3.3", np.__version__
assert scipy.__version__ == "1.16.3", scipy.__version__
start = dt.datetime(2016, 12, 24)
origin = dt.datetime(2000, 1, 1, 12)
count = 112
lag = 24120_000_000_000

def tai_ns(utc):
    offset = 36 if utc < dt.datetime(2017, 1, 1) else 37
    span = utc - origin
    return (span.days * 86400 + span.seconds + offset) * 1_000_000_000

edges = [tai_ns(start + dt.timedelta(hours=3*i)) for i in range(count+1)]
ap = [35 + 12*math.sin(i*.28) + 5*math.cos(i*.63) + i*.15 for i in range(count)]
kp = [round((3 + .7*math.cos(i*.21) + .4*math.sin(i*.67))*3)/3 for i in range(count)]
ap[20:28] = [0, 0, 400, 0, 100, 399, 1, 0]
kp[30:37] = [0, 9, 0, 26/3, 1/3, 9, 0]
ap[40:46] = [None, 17, None, 33, 99, None]
kp[50:56] = [None, 4, None, 2, 5, None]
lines = ["# SciPy 1.16.3 / NumPy 2.3.3 natural cubic primitive derivative + cell-mean scaling",
         "# D: day_index slot_index Ap Kp (-1=missing); first date 2016-12-24; flux/center81=150",
         "# C: source(0=Ap,1=Kp) query_TAI_ns selected_index; JR 24120 s lag included",
         "# Q: source bin_start_TAI_ns bin_end_TAI_ns mean (query_TAI_ns weight)*5"]
for i in range(count):
    lines.append(f"D {i//8} {i%8} {ap[i] if ap[i] is not None else -1:.17g} {kp[i] if kp[i] is not None else -1:.17g}")
points, weights = leggauss(5)
limited = 0
for source, values in enumerate((ap, kp)):
    bound = (400, 9)[source]
    runs, begin = [], 0
    while begin < count:
        if values[begin] is None:
            begin += 1
            continue
        end = begin + 1
        while end < count and values[end] is not None:
            end += 1
        runs.append((begin, end)); begin = end
    for begin, end in runs:
        cumulative = np.r_[0.0, np.cumsum(values[begin:end])]
        derivative = CubicSpline(np.arange(end-begin+1), cumulative,
                                 bc_type="natural", extrapolate=False).derivative()
        for i in range(begin, end):
            y = values[i]
            polynomial = Polynomial(derivative.c[:, i-begin][::-1])
            candidates = [0., 1.] + [r.real for r in polynomial.deriv().roots()
                                     if np.isreal(r) and 0 < r.real < 1]
            low, high = min(polynomial(candidates)), max(polynomial(candidates))
            factors = [1.]
            if low < 0: factors.append(y / (y-low))
            if high > bound: factors.append((bound-y)/(high-y))
            theta = 0. if y in (0, bound) else min(factors)
            limited += theta < 1
            profile = Polynomial([y]) + theta * (polynomial - y)
            # The primitive interpolation and scaling both retain the cell integral.
            integral = profile.integ()
            assert abs(integral(1)-integral(0)-y) < 2e-11
            assert min(profile(candidates)) > -2e-11
            assert max(profile(candidates)) < bound + 2e-11
            duration = edges[i+1]-edges[i]
            offsets = {0, 1, 123456789, duration//2, duration*3//4, duration-1}
            if duration == 10801_000_000_000:
                offsets |= {10800_000_000_000-1, 10800_000_000_000, 10800_500_000_000}
            for offset in sorted(offsets):
                at = edges[i]+offset+lag
                expected = float(profile(offset/duration))
                lines.append(f"C {source} {at} {expected:.17g}")
            quad = []
            for x,w in zip(points,weights):
                ns = edges[i]+round((float(x)+1)/2*duration)+lag
                quad += [str(ns), format(float(w)/2,'.17g')]
            lines.append(f"Q {source} {edges[i]} {edges[i+1]} {y:.17g} "+' '.join(quad))

ap_table = [0,2,3,4,5,6,7,9,12,15,18,22,27,32,39,48,56,67,80,94,111,132,154,179,207,236,300,400]
records = [row.split() for row in lines if row.startswith('C ')][::19]
inputs, metadata = [], []
for j,row in enumerate(records):
    source,ns,index = int(row[1]),int(row[2]),float(row[3])
    index = min((400,9)[source],max(0,index))
    kp_value = float(np.interp(index,ap_table,np.arange(28)/3)) if source == 0 else index
    offset = 36 if ns < tai_ns(dt.datetime(2017,1,1)) else 37
    utc = origin + dt.timedelta(seconds=ns//1_000_000_000-offset)
    seconds = utc.hour*3600+utc.minute*60+utc.second+(ns%1_000_000_000)*1e-9
    leap_begin = tai_ns(dt.datetime(2017,1,1))-1_000_000_000
    if leap_begin <= ns < leap_begin+1_000_000_000:
        utc=dt.datetime(2016,12,31);seconds=86400+(ns-leap_begin)*1e-9
    mjd=21544.5+(utc.date()-dt.date(2000,1,1)).days+seconds/86400
    height=(100_000,126_000,400_000,1_000_000)[j%4]
    latitude=(-.7,.2,.8)[j%3];angle=1.2;declination=.2
    inputs.append(' '.join(format(v,'.17g') for v in (mjd,height,latitude,angle,declination,150,150,kp_value)))
    metadata.append((source,ns,utc.year,utc.month,utc.day,seconds,height,latitude,angle,declination))
result=subprocess.run([sys.argv[2]],input='\n'.join(inputs)+'\n',text=True,encoding='ascii',stdout=subprocess.PIPE,check=True)
densities=result.stdout.splitlines();assert len(densities)==len(metadata)
lines.append('# R: source TAI_ns year month day UTC_seconds height_m latitude_rad hour_angle_rad sun_declination_rad GMAT_density_kg_m3')
for meta,rho in zip(metadata,densities):
    assert float(rho)>0
    lines.append('R '+' '.join(str(v) if isinstance(v,int) else format(v,'.17g') for v in meta)+' '+rho)
lines.append(f'# limited bins: {limited}')
Path(sys.argv[1]).write_text('\n'.join(lines)+'\n',encoding='ascii',newline='\n')
print({tag:sum(s.startswith(tag+' ') for s in lines) for tag in ('D','C','Q','R')},'limited',limited)
