"""FXM reference: SciPy 1.16.3 linear interpolation and original NASA GMAT JR.

Run: python tools/validation/fxm_weather_reference.py OUTPUT_DIR GMAT_DRIVER
No OrbitForge imports. Synthetic daily solar distances span 0.97..1.035 AU;
these deliberately vary faster than the real Sun to expose scaling/lag mistakes.
GMAT driver/source provenance: jacchia_roberts_weather_reference.cpp and
 density_sources.json. Values cover the 2016 leap second and FXM forecasts.
"""
import datetime as dt
from pathlib import Path
import subprocess
import sys
import numpy as np
import scipy
from scipy.interpolate import interp1d

assert scipy.__version__ == "1.16.3", scipy.__version__
assert np.__version__ == "2.3.3", np.__version__
AU = 149597870700.0
origin = dt.datetime(2000, 1, 1, 12)
start = dt.datetime(2016, 12, 24)

def tai_ns(utc):
    offset = 36 if utc < dt.datetime(2017, 1, 1) else 37
    span = utc - origin
    return (span.days * 86400 + span.seconds + offset) * 1_000_000_000

n = 14
nodes = [tai_ns(start + dt.timedelta(days=i, hours=20)) for i in range(n)]
raw = np.array([110 + 7*i for i in range(n)], dtype=float)
average = np.array([140 + .9*i for i in range(n)])
distances = AU * (.97 + .005*np.arange(n))
observed = raw * (AU / distances)**2
means = average * (AU / distances)**2
# Reduce the time origin before converting integers to double seconds.
xs = np.array([(ns-nodes[0])*1e-9 for ns in nodes])
flux = interp1d(xs, observed, bounds_error=True)
mean = interp1d(xs, means, bounds_error=True)
lines = ["# SciPy 1.16.3 interp1d; synthetic geometric distance; NASA GMAT c9b78bd",
         "# N: day_index TAI_ns distance_m observed_F107 observed_center81",
         "# S: model(0=JR,1=NRL) TAI_ns daily_F107 center81 Ap Kp",
         "# R: TAI_ns year month day UTC_seconds height_m latitude_rad hour_angle_rad sun_declination_rad GMAT_density_kg_m3"]
for i in range(n):
    lines.append(f"N {i} {nodes[i]} {distances[i]:.17g} {observed[i]:.17g} {means[i]:.17g}")
fxm = ['BEGIN OBSERVED']
for i in range(10):
    day = start + dt.timedelta(days=i)
    fields = [' '] * 78
    def put(a, b, text):
        assert len(text) == b-a
        fields[a:b] = text
    put(0, 8, day.strftime('%Y%m%d'))
    put(8, 12, '   1'); put(12, 14, ' 1')
    for slot in range(8):
        put(14+2*slot, 16+2*slot, '30')
        put(33+3*slot, 36+3*slot, f"{7+5*(i%4):3d}")
    put(30,33,'240'); put(57,60,f"{7+5*(i%4):3d}")
    put(60,63,'0.0'); put(63,64,'0'); put(64,67,'  0')
    put(67,72,f"{raw[i]:5.1f}"); put(72,73,'0'); put(73,78,f"{average[i]:5.1f}")
    fxm.append(''.join(fields))
fxm += ['END OBSERVED','BEGIN F10_PREDICT']
for i in range(10,n):
    fxm.append(f"{start+dt.timedelta(days=i):%Y%m%d} {int(raw[i]):3d} {average[i]:5.1f}")
fxm += ['END F10_PREDICT','BEGIN AP_PREDICT']
for i in range(10,n):
    fxm.append(f"{start+dt.timedelta(days=i):%Y%m%d} {7+5*(i%4):3d}")
fxm += ['END AP_PREDICT']
queries = set()
for i in range(4,12):
    for delta in (-1,0,1,43200_000_000_000):
        queries.add(nodes[i]+delta)
midnight=tai_ns(dt.datetime(2017,1,1))
queries.update(midnight+delta for delta in (-1_000_000_000,-500_000_000,-1,0,1))
ap_table = [0,2,3,4,5,6,7,9,12,15,18,22,27,32,39,48,56,67,80,94,111,132,154,179,207,236,300,400]
inputs, metadata = [], []
for model in (0,1):
    for j, ns in enumerate(sorted(queries)):
        f=float(flux((ns-86400_000_000_000-nodes[0])*1e-9))
        avg=float(mean((ns-(86400_000_000_000 if model==0 else 0)-nodes[0])*1e-9))
        geom=ns-(24120_000_000_000 if model==0 else 0)
        day_index=max(i for i in range(n) if tai_ns(start+dt.timedelta(days=i))<=geom)
        ap=7+5*(day_index%4); kp=float(np.interp(ap,ap_table,np.arange(28)/3))
        lines.append(f"S {model} {ns} {f:.17g} {avg:.17g} {ap} {kp:.17g}")
        if model != 0:
            continue
        offset=36 if ns<midnight else 37
        utc=origin+dt.timedelta(seconds=ns//1_000_000_000-offset)
        seconds=utc.hour*3600+utc.minute*60+utc.second+(ns%1_000_000_000)*1e-9
        if midnight-1_000_000_000<=ns<midnight:
            utc=dt.datetime(2016,12,31);seconds=86400+(ns-(midnight-1_000_000_000))*1e-9
        mjd=21544.5+(utc.date()-dt.date(2000,1,1)).days+seconds/86400
        height=(100000,126000,400000,1000000)[j%4]
        lat=(-.7,.2,.8)[j%3]; angle=1.2; decl=.2
        inputs.append(' '.join(format(v,'.17g') for v in (mjd,height,lat,angle,decl,f,avg,kp)))
        metadata.append((ns,utc.year,utc.month,utc.day,seconds,height,lat,angle,decl))
result=subprocess.run([sys.argv[2]],input='\n'.join(inputs)+'\n',text=True,encoding='ascii',stdout=subprocess.PIPE,check=True)
rhos=result.stdout.splitlines();assert len(rhos)==len(metadata)
for row,rho in zip(metadata,rhos):
    assert float(rho)>0
    lines.append('R '+' '.join(str(v) if isinstance(v,int) else format(v,'.17g') for v in row)+' '+rho)
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
(out/'weather_fxm.txt').write_text('\n'.join(lines)+'\n',encoding='ascii',newline='\n')
(out/'weather_fxm.fxm').write_text('\n'.join(fxm)+'\n',encoding='ascii',newline='\n')
