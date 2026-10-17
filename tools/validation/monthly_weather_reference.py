"""Independent monthly weather reference using NumPy 2.3.3, SciPy 1.16.3 and original GMAT.

python tools/validation/monthly_weather_reference.py OUTPUT_DIR GMAT_DRIVER
No OrbitForge imports. CSSI nominal nodes are on day 15; CelesTrak monthly nodes
are on day 1. Flux nodes use 20 UTC, geomagnetic nodes midnight. CSSI averages
use a NumPy 81-day centered convolution before daily-node interpolation.
"""
import csv
import datetime as dt
from pathlib import Path
import subprocess
import sys
import numpy as np
import scipy
from scipy.interpolate import interp1d
assert np.__version__ == '2.3.3'
assert scipy.__version__ == '1.16.3'
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
origin=dt.datetime(2000,1,1,12)
def tai(utc):
    span=utc-origin;offset=36 if utc<dt.datetime(2017,1,1) else 37
    return (span.days*86400+span.seconds+offset)*1_000_000_000
base=tai(dt.datetime(2015,11,15))
def curve(nodes,values):
    f=interp1d(np.array([(n-base)/1e9 for n in nodes]),np.array(values),kind='linear',bounds_error=True)
    return lambda ns: float(f((ns-base)/1e9))
months=[(2015+(10+i)//12,(10+i)%12+1) for i in range(18)]
flux=[90,120,85,160,105,200,140,180,110,155,95,170,130,80,145,100,190,150]
aps=[4,25,8,40,10,80,15,35,5,45,20,60,12,30,7,50,10,25]
cssi=['2015 11 2017 4 2015 10 18']
for (y,m),f,a in zip(months,flux,aps):cssi.append(f'{y} {m} {f+20} {f} {a+3} {a}')
(out/'weather_cssi.dat').write_text('\n'.join(cssi)+'\n',encoding='ascii',newline='\n')
dates=[dt.datetime(y,m,15) for y,m in months]
fnodes=[tai(d+dt.timedelta(hours=20)) for d in dates];anodes=[tai(d) for d in dates]
cssi_flux=curve(fnodes,flux);cssi_ap=curve(anodes,aps)
calendar=[dates[0]+dt.timedelta(days=i,hours=20) for i in range((dates[-1]-dates[0]).days+1)]
daily=np.array([cssi_flux(tai(d)) for d in calendar])
averages=np.convolve(daily,np.ones(81)/81,mode='valid')
cssi_average=curve([tai(d) for d in calendar[40:-40]],averages)
header=('DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,'
        'AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,'
        'F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81')
raw=[]
for i in range(4):raw.append((dt.datetime(2016,11,21+i),150,145,15,3,False))
for (y,m),f,avg,a,k in zip([(2016,12),(2017,1),(2017,2),(2017,3),(2017,4)],
                          [180,95,160,80,200],[155,160,120,135,150],[35,10,75,5,20],[4,2,5,1,3]):
    raw.append((dt.datetime(y,m,1),f,avg,a,k,True))
with (out/'weather_monthly.csv').open('w',encoding='ascii',newline='') as file:
    writer=csv.writer(file,lineterminator='\n');writer.writerow(header.split(','))
    for d,f,avg,a,k,monthly in raw:
        row=[d.date().isoformat(),1,1]+(['']*8 if monthly else [30]*8)+[k*80]
        row+=(['']*8 if monthly else [15]*8)+[a,0,0,0,f,500,'PRM' if monthly else 'OBS',avg,600,500,600]
        assert len(row)==31;writer.writerow(row)
monthly_flux=curve([tai(r[0]+dt.timedelta(hours=20)) for r in raw],[r[1] for r in raw])
monthly_average=curve([tai(r[0]+dt.timedelta(hours=20)) for r in raw],[r[2] for r in raw])
monthly_ap=curve([tai(r[0]) for r in raw],[r[3] for r in raw])
monthly_kp=curve([tai(r[0]) for r in raw],[r[4] for r in raw])
ap_table=[0,2,3,4,5,6,7,9,12,15,18,22,27,32,39,48,56,67,80,94,111,132,154,179,207,236,300,400]
kp_table=np.arange(28)/3
lines=['# NumPy 2.3.3 / SciPy 1.16.3 monthly interpolation + original GMAT JR',
       '# S: dataset(0=CSSI,1=CelesTrak) model(0=JR,1=NRL) update source epoch_TAI_ns F107 F107_average Kp Ap history_present history[7]',
       '# R: dataset model update source epoch_TAI_ns height_m latitude_rad hour_angle_rad sun_declination_rad density_kg_m3']
records=[]
for dataset in (0,1):
    seeds=([dt.datetime(2016,2,15),dt.datetime(2016,2,15,20),dt.datetime(2016,2,29,12),
            dt.datetime(2016,12,15),dt.datetime(2016,12,15,20),dt.datetime(2017,1,1),
            dt.datetime(2017,1,15),dt.datetime(2017,1,15,20),dt.datetime(2017,2,1,12)] if dataset==0 else
           [dt.datetime(2016,11,24),dt.datetime(2016,11,26,20),dt.datetime(2016,12,1),
            dt.datetime(2016,12,1,20),dt.datetime(2017,1,1),dt.datetime(2017,1,1,20),dt.datetime(2017,2,1)])
    epochs={tai(d)+delta for d in seeds for delta in (-1,0,1)}
    leap=tai(dt.datetime(2017,1,1))-1_000_000_000
    epochs|={leap-1,leap,leap+500_000_000}
    # Hit a JR geomagnetic node and a lagged flux node on both sides.
    for lag in (24120_000_000_000,86400_000_000_000):
        epochs|={tai(dt.datetime(2017,1,1))+lag+delta for delta in (-1,0,1)}
    fcurve,avgcurve=(cssi_flux,cssi_average) if dataset==0 else (monthly_flux,monthly_average)
    for model in (0,1):
      for update in range(4):
       for source in (0,1):
        use_ap=dataset==0 or source==0
        def geomag(ns):
            if use_ap:
                a=(cssi_ap if dataset==0 else monthly_ap)(ns)
                return float(np.interp(a,ap_table,kp_table)),a
            k=monthly_kp(ns)
            return k,float(np.interp(k,kp_table,ap_table))
        for ns in sorted(epochs):
            k,a=geomag(ns-(24120_000_000_000 if model==0 else 0))
            f=fcurve(ns-86400_000_000_000)
            avg=avgcurve(ns-(86400_000_000_000 if model==0 else 0))
            history=[0.]*7;active=int(model==1 and update!=0)
            if active:
                history=[a]+[geomag(ns-i*10800_000_000_000)[1] for i in range(4)]
                history += [float(np.mean([geomag(ns-i*10800_000_000_000)[1] for i in range(first,first+8)])) for first in (4,12)]
            prefix=[dataset,model,update,source,ns]
            lines.append('S '+' '.join(str(v) for v in prefix)+' '+' '.join(format(v,'.17g') for v in (f,avg,k,a))+' '+str(active)+' '+' '.join(format(v,'.17g') for v in history))
            records.append((prefix,f,avg,k))
inputs=[];meta=[]
for j,(prefix,f,avg,k) in enumerate([r for r in records if r[0][1]==0][::17]):
    ns=prefix[-1];offset=36 if ns<tai(dt.datetime(2017,1,1)) else 37
    utc=origin+dt.timedelta(seconds=ns//1_000_000_000-offset)
    seconds=utc.hour*3600+utc.minute*60+utc.second+ns%1_000_000_000*1e-9
    leap=tai(dt.datetime(2017,1,1))-1_000_000_000
    if leap<=ns<leap+1_000_000_000:utc=dt.datetime(2016,12,31);seconds=86400+(ns-leap)*1e-9
    jd=21544.5+(utc.date()-dt.date(2000,1,1)).days+seconds/86400
    h=(100000,126000,400000,1000000)[j%4];lat=(-.7,.2,.8)[j%3];angle=1.2;decl=.2
    inputs.append(' '.join(format(v,'.17g') for v in (jd,h,lat,angle,decl,f,avg,k)))
    meta.append(prefix+[h,lat,angle,decl])
result=subprocess.run([sys.argv[2]],input='\n'.join(inputs)+'\n',text=True,encoding='ascii',stdout=subprocess.PIPE,check=True)
densities=result.stdout.splitlines();assert len(densities)==len(meta)
for m,rho in zip(meta,densities):lines.append('R '+' '.join(str(v) if isinstance(v,int) else format(v,'.17g') for v in m)+' '+rho)
(out/'weather_monthly.txt').write_text('\n'.join(lines)+'\n',encoding='ascii',newline='\n')
print('weather rows',len(records),'GMAT rows',len(meta))
