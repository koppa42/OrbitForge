"""Independent multi-occultor reference: SciPy 1.16.3 QUADPACK and DOP853.

No OrbitForge library or boundary-arc formula is used. Every solar-disk slice
is clipped against a sorted union of vertical occultor intervals.
"""
import argparse
import json
import math
from pathlib import Path
import random
import warnings
import numpy as np
import scipy
from scipy.integrate import quad, solve_ivp, IntegrationWarning


def visible(disks, eps=2e-12):
    disks = [v for v in disks if math.hypot(v[0], v[1]) < 1 + v[2]]
    if any(math.hypot(x, y) + 1 <= r for x, y, r in disks):
        return 0.0
    if not disks:
        return 1.0
    circles = [(0., 0., 1.), *disks]
    knots = [-1., 1.]
    for x, y, r in disks:
        knots.extend(t for t in (x-r, x+r) if -1 < t < 1)
    for i, (x, y, r) in enumerate(circles):
        for u, v, s in circles[i+1:]:
            dx, dy = u-x, v-y
            d = math.hypot(dx, dy)
            if d == 0 or d >= r+s or d <= abs(r-s):
                continue
            along = (r*r-s*s+d*d)/(2*d)
            height = math.sqrt(max(0., (r-along)*(r+along)))
            centre = x + along*dx/d
            knots.extend(t for t in (centre-height*dy/d, centre+height*dy/d) if -1 < t < 1)
    knots = sorted(set(knots))

    def strip(x):
        sun = math.sqrt(max(0., (1-x)*(1+x)))
        intervals = []
        for cx, cy, r in disks:
            dx = x-cx
            squared = (r-dx)*(r+dx)
            if squared > 0:
                height = math.sqrt(squared)
                a, b = max(-sun, cy-height), min(sun, cy+height)
                if a < b:
                    intervals.append((a,b))
        end, total = -sun, 0.
        for a,b in sorted(intervals):
            if b > end:
                total += b-max(a,end)
                end = b
        return total
    area = 0.
    for a,b in zip(knots,knots[1:]):
        if b-a <= 1e-15:
            continue
        mid, half = (a+b)/2, (b-a)/2
        value, error = quad(lambda t: strip(mid+half*math.sin(t))*half*math.cos(t),
                            -math.pi/2, math.pi/2, epsabs=eps, epsrel=eps, limit=150)
        if error > 2e-10:
            raise RuntimeError(f"Unresolved quadrature error {error}")
        area += value
    return min(1., max(0., 1-area/math.pi))


def geometry_cases():
    cases = [[], [(0,0,.5)], [(0,0,2)], [(0,0,.5)]*10,
             [(i*.19-.85, 0, .08) for i in range(10)],
             [(.1*i, -.04*i, .8-.05*i) for i in range(10)],
             [(math.cos(i*math.pi/3)*.7, math.sin(i*math.pi/3)*.7, .48) for i in range(6)],
             [(2*math.cos(i),2*math.sin(i),1) for i in range(10)]]
    rng = random.Random(0x53525033)
    for count in range(2,11):
        for _ in range(16):
            cases.append([(rng.uniform(-1.5,1.5),rng.uniform(-1.5,1.5),rng.uniform(.04,.85))
                          for _ in range(count)])
    for radius in (10.,50.,150.,300.):
        for offset in (-.9,-.3,0.,.3,.9):
            cases.append([(radius+offset,.5,radius), *[(rng.uniform(-1,1),rng.uniform(-1,1),.15)
                                                       for _ in range(9)]])
    for offset in (-1e-9,0,1e-9):
        cases.append([(1+offset,0,.5),(.0,0,.5),(-1-offset,0,.5)])
    return cases


AU=149597870700.
MU=3.986004415e14
SUN_RADIUS=695700000.
PLANET_RADII=(9e7,1.25e8,8e7)


def centres(t):
    return ((3e10, 9e7*math.sin(t/3600+.1), 2e7),
            (4e10, -1.4e8+30000*t, -3e7),
            (5e10, 1e8*math.sin(t/2400+.5), 1.1e8))


def illumination(r,t):
    to_sun=np.array((AU,0.,0.))-r
    distance=np.linalg.norm(to_sun)
    axis=to_sun/distance
    x=np.cross(axis,(0,0,1.));x/=np.linalg.norm(x)
    y=np.cross(axis,x)
    alpha=math.asin(SUN_RADIUS/distance)
    disks=[]
    for centre,radius in zip(((0.,0.,0.),*centres(t)),(6378137.,*PLANET_RADII)):
        d=np.array(centre)-r;length=np.linalg.norm(d)
        if np.dot(d,axis)<=0 or length>=distance:
            continue
        unit=d/length
        angle=math.atan2(np.linalg.norm(np.cross(axis,unit)),np.dot(axis,unit))
        beta=math.asin(radius/length)
        if angle>=alpha+beta:
            continue
        dx,dy=np.dot(unit,x),np.dot(unit,y)
        scale=angle/(math.hypot(dx,dy)*alpha) if dx or dy else 0.
        disks.append((dx*scale,dy*scale,beta/alpha))
    return visible(disks,eps=8e-12),to_sun,distance


def dynamics(t,state):
    r=state[:3]
    light,s,d=illumination(r,t)
    a=-MU*r/np.linalg.norm(r)**3-4.56e-6*1.7*.2*(AU/d)**2*light*s/d
    return np.r_[state[3:],a]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--trajectories',action='store_true')
    args=parser.parse_args()
    if scipy.__version__!='1.16.3':
        raise RuntimeError('Pinned reference requires SciPy 1.16.3')
    warnings.simplefilter('error',IntegrationWarning)
    rows=['# SciPy 1.16.3 QUADPACK independent vertical-slice union; no OrbitForge imports']
    if args.trajectories:
        rows+=['# DOP853 point Earth, fixed geometric Sun, three synthetic moving spherical planets',
               '# initial x y z vx vy vz; signed dt; final x y z vx vy vz']
        convergence=[]
        for radius,speed in ((7e6,math.sqrt(MU/7e6)),(4.2e7,math.sqrt(MU/4.2e7))):
            initial=np.array((radius,0.,0.,0.,speed*.8,speed*.6))
            for dt in (-6000.,6000.):
                solutions=[]
                for max_step in (5.,2.5):
                    value=solve_ivp(dynamics,(0.,dt),initial,method='DOP853',
                                     atol=[1e-8]*3+[1e-11]*3,rtol=2.3e-14,max_step=max_step)
                    if not value.success:raise RuntimeError(value.message)
                    solutions.append(value.y[:,-1])
                error=np.abs(solutions[0]-solutions[1])
                if max(error[:3])>1e-4 or max(error[3:])>1e-7:
                    raise RuntimeError(f'Unconverged independent trajectory {error}')
                convergence.append(error.tolist())
                rows.append(' '.join(format(v,'.17g') for v in (*initial,dt,*solutions[-1])))
                print(json.dumps({'radius':radius,'dt':dt,'convergence':error.tolist()}),flush=True)
        print(json.dumps({'max_convergence':np.max(convergence,axis=0).tolist()}))
    else:
        rows+=['# count, visible fraction, followed by count triples x y radius in solar angular units']
        maximum=0.
        for disks in geometry_cases():
            expected=visible(disks)
            check=visible(disks,eps=1e-11)
            maximum=max(maximum,abs(expected-check))
            rows.append(' '.join([str(len(disks)),format(expected,'.17g'),
                                  *(format(v,'.17g') for disk in disks for v in disk)]))
        if maximum>2e-10:raise RuntimeError('Quadrature convergence failed')
        print(json.dumps({'cases':len(rows)-2,'max_convergence':maximum}))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(rows)+'\n',encoding='utf-8',newline='\n')


if __name__=='__main__':
    main()
