"""High-altitude GMAT Roberts equations in 80/100 decimal digits (mpmath).

Coefficient arrays are read from hash-checked GMAT c9b78bd source. Evaluates the
original temperature SUBTRACTIONS, independently of the production exponential
ratio rewrite. mpmath.diff computes Cartesian density derivatives at extra
precision. This is a high-precision transcription, not a GMAT executable.
"""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import mpmath as mp


def load_arrays(source):
    record = next(x for x in json.loads(Path(__file__).with_name('density_sources.json').read_text())
                  if x['file'] == 'JacchiaRobertsAtmosphere.cpp')
    assert hashlib.sha256(source.read_bytes()).hexdigest() == record['sha256']
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', source.read_text(), flags=re.S)
    arrays = {}
    for name in ('CON_L','MOL_MASS','CON_DEN'):
        match = re.search(r'::'+name+r'(?:\[[^\]]+\])+\s*=\s*\{(.*?)\};',text,re.S)
        values = re.findall(r'[-+]?(?:\d+\.?(?:\d*)?|\.\d+)(?:[eE][-+]?\d+)?',match.group(1))
        # GMAT's unsuffixed literals round to binary64 before assignment to Real.
        arrays[name] = tuple(mp.mpf(float(x)) for x in values)
    return arrays


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--legacy-reference', type=Path)
    parser.add_argument('--legacy-output', type=Path)
    args=parser.parse_args()
    assert bool(args.legacy_reference) == bool(args.legacy_output)

    def generate(dps):
        with mp.workdps(dps):
            co=load_arrays(args.source)
            # Match binary64 model constants while doing arithmetic at high precision.
            c=lambda x:mp.mpf(float(x))
            pi=mp.pi
            a=c(6378137);f=c(1/298.257223563);e2=f*(2-f);b=a*(1-f)
            polar=b/1000
            sun=tuple(map(mp.mpf,(1.4e11,-5e10,3e10)))
            sun_lon=mp.atan2(sun[1],sun[0]);dec=mp.atan2(sun[2],mp.hypot(sun[0],sun[1]))
            def density(r,approximate,mode,geometry=None,weather=None,mjd=None):
                if geometry is None:
                    x,y,z=r;p=mp.hypot(x,y)
                    lat=mp.atan2(z,p)
                    for _ in range(50):
                        n=a/mp.sqrt(1-e2*mp.sin(lat)**2)
                        lat=mp.atan2(z+e2*n*mp.sin(lat),p)
                    n=a/mp.sqrt(1-e2*mp.sin(lat)**2)
                    height=p/mp.cos(lat)-n
                    if approximate:
                        radius=mp.sqrt(x*x+y*y+z*z)
                        height=radius-b/mp.sqrt(1-e2*p*p/(radius*radius))
                    hour=mp.atan2(sun[0]*y-sun[1]*x,sun[0]*x+sun[1]*y)
                    declination=dec
                else:
                    lat,height,hour,declination=map(mp.mpf,geometry)
                h=height/1000
                flux,avg,kp=((150,150,3),(180,165,5.5),(70,80,0))[mode] if weather is None else weather
                day=((c(21603.5)+c(48192.5)/86400) if mjd is None else mp.mpf(mjd))
                day=(day-c(6204.5))/c(365.2422)
                tau_season=day+c(.09544)*((mp.mpf('.5')*(1+mp.sin(2*pi*day+c(6.035))))**c(1.65)-mp.mpf('.5'))
                alpha=mp.sin(4*pi*tau_season+c(4.259))
                annual=c(.02835)+(c(.3817)+c(.17829)*mp.sin(2*pi*tau_season+c(4.137)))*alpha
                tbase=379+c(3.24)*avg+c(1.3)*(flux-avg)
                theta=abs(lat+declination)/2;eta=abs(lat-declination)/2
                tau=hour-c(.64577182325)+c(.10471975512)*mp.sin(hour+c(.75049157836))
                if tau < -pi:tau+=2*pi
                elif tau > pi:tau-=2*pi
                th22=mp.sin(theta)**c(2.2)
                t1=tbase*(1+c(.3)*(th22+mp.cos(tau/2)**3*(mp.cos(eta)**c(2.2)-th22)))
                tinf=t1+(14*kp+c(.02)*mp.exp(kp) if h<200 else 28*kp+c(.03)*mp.exp(kp))
                tx=c(371.6678)+c(.0518806)*tinf-c(294.3505)*mp.exp(-c(.00216222)*tinf)
                lsum=mp.polyval(list(reversed(co['CON_L'])),tinf)
                def temperature(alt):
                    return tinf-(tinf-tx)*mp.exp(-(tx-183)/(tinf-tx)*(alt-125)/35*lsum/(polar+alt))
                temp=temperature(h);t500=temperature(500)
                total=mp.mpf(0)
                for i,mass in enumerate(co['MOL_MASS']):
                    gamma=35*mass*c(9.80665)*polar**2*(tinf-tx)/(c(8.31432)*lsum*tinf*(tx-183)*(polar+125))
                    exp1=1+gamma-(c(.38) if i==2 else 0)
                    if i<5:
                        logdi=mp.polyval(list(reversed(co['CON_DEN'][7*i:7*i+7])),tinf)
                        di=10**logdi/c(6.022045e23)
                        helium=10**(c(4.9914)*abs(declination)*(mp.sin(pi/4-lat*mp.sign(declination)/2)**3-c(.35355))/pi) if i==2 else 1
                        total+=helium*mass*di*(tx/temp)**exp1*((tinf-temp)/(tinf-tx))**gamma
                    elif h>500:
                        total+=mass*10**(c(73.13)-(c(39.4)-c(5.5)*mp.log10(t500))*mp.log10(t500))*(t500/temp)**exp1*((tinf-temp)/(tinf-t500))**gamma/c(6.022045e23)
                geocor=c(.012)*kp+c(.000012)*mp.exp(kp) if h<200 else 0
                seasonal=(c(5.876e-7)*h**c(2.331)+c(.06328))*mp.exp(-c(.002868)*h)*annual
                slat=c(.014)*(h-90)*mp.sin(2*pi*day+c(1.72))*mp.sin(lat)*abs(mp.sin(lat))*mp.exp(-c(.0013)*(h-90)**2)
                return 1000*total*10**(geocor+seasonal+slat)

            rows=[]
            for mode in range(3):
                for approx in range(2):
                    for h in (140000,199900,200100,400000,499900,500100,1000000,1800000,2499000):
                        for lat,lon in ((-80,.4),(0,.7),(45,-2),(80,2.9)):
                            lat=math.radians(lat)
                            n=6378137/math.sqrt(1-float(e2)*math.sin(lat)**2)
                            r=((n+h)*math.cos(lat)*math.cos(lon),(n+h)*math.cos(lat)*math.sin(lon),(n*(1-float(e2))+h)*math.sin(lat))
                            point=tuple(map(mp.mpf,r))
                            rho=density(point,approx,mode)
                            gradient=[]
                            for j in range(3):
                                def shifted(dx):
                                    q=list(point);q[j]+=dx
                                    return density(q,approx,mode)
                                gradient.append(mp.diff(shifted,0))
                            rows.append((mode,approx,*r,float(rho),*map(float,gradient)))
            legacy=[]
            if args.legacy_reference:
                for line in args.legacy_reference.read_text().splitlines():
                    if not line or line.startswith('#'):continue
                    row=list(map(float,line.split()))
                    if row[4]>125000:
                        date=datetime.date(*map(int,row[:3]))
                        mjd=c(21544.5)+(date-datetime.date(2000,1,1)).days+c(row[3])/86400
                        row[12]=float(density(None,False,0,geometry=(row[5],row[4],row[7],row[8]),
                                              weather=tuple(map(mp.mpf,row[9:12])),mjd=mjd))
                    legacy.append(tuple(row))
            return rows,legacy

    rows,legacy=generate(80)
    verification,legacy_verification=generate(100)
    assert legacy==legacy_verification
    assert rows==verification, '80/100 digit results must round to identical binary64 values'
    out=['# GMAT c9b78bd Roberts equations, mpmath 80/100 digits; original temperature subtraction',
         '# UTC 2000-02-29 13:23:12.5; Sun=(1.4e11,-5e10,3e10); Cartesian mpmath.diff',
         '# mode approx r[3] rho grad_rho[3]']
    out.extend(' '.join(format(x,'.17g') for x in row) for row in rows)
    args.output.write_text('\n'.join(out)+'\n',encoding='utf-8',newline='\n')
    if args.legacy_output:
        text=['# GMAT c9b78bd: original double model <=125km, 80/100-digit original equations >125km',
              '# year month day UTC_seconds altitude_m latitude_rad longitude_rad solar_hour_angle_rad sun_declination_rad f107 f107_average kp density_kg_m3']
        text.extend(' '.join(format(x,'.17g') for x in row) for row in legacy)
        args.legacy_output.write_text('\n'.join(text)+'\n',encoding='utf-8',newline='\n')
    print(len(rows),'gradient rows;',len(legacy),'legacy density rows; 80/100-digit binary64 agreement')


if __name__=='__main__':
    main()
