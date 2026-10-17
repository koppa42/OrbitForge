"""Independent SciPy 1.16.3 QUADPACK disk union + eighth-order Cartesian differences.

No OrbitForge import or boundary-arc/analytic-derivative formula. Compares two
stencil widths and two quadrature tolerances before accepting any reference.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import warnings
import numpy as np
import scipy
from scipy.integrate import IntegrationWarning
from generate_srp_multi_reference import visible

AU=149597870700.
RS=695700000.
PRESSURE=4.56e-6


def geometry(r, sun, bodies):
    s=sun-r
    distance=np.linalg.norm(s)
    n=s/distance
    # An independent plane chart: never uses the production |z|=0.8 switch.
    u=np.cross(n,(0.,0.,1.)); u/=np.linalg.norm(u)
    v=np.cross(n,u)
    alpha=math.asin(RS/distance)
    disks=[]
    for radius,centre in bodies:
        b=centre-r; d=np.linalg.norm(b); unit=b/d
        if np.dot(b,n)<=0 or d>=distance:
            continue
        separation=math.atan2(np.linalg.norm(np.cross(n,unit)),np.dot(n,unit))
        beta=math.asin(radius/d)
        tx,ty=np.dot(unit,u),np.dot(unit,v)
        scale=separation/(math.hypot(tx,ty)*alpha) if tx or ty else 0.
        disks.append((tx*scale,ty*scale,beta/alpha))
    return disks,s,distance


def values(x,bodies,k,eps):
    disks,s,d=geometry(x[:3],x[3:],bodies)
    light=visible(disks,eps=eps)
    flux=-PRESSURE*(AU/d)**2*light*s/d
    return np.r_[light,k*flux],flux


def differential(x,bodies,k,steps,eps):
    jac=np.zeros((4,6))
    for j in range(6):
        step=steps[j//3]
        for offset,weight in ((1,4/5),(2,-1/5),(3,4/105),(4,-1/280)):
            delta=np.zeros(6); delta[j]=offset*step
            plus,_=values(x+delta,bodies,k,eps)
            minus,_=values(x-delta,bodies,k,eps)
            jac[:,j]+=weight*(plus-minus)/step
    return jac


def configurations():
    rng=random.Random(0x53525044)
    cases=[(0,[]),(0,[(0.,0.,.45)]),(0,[(0.,0.,2.)]),
           (0,[(.2,.1,.5)]*9),
           (0,[(i*.19-.8,0.,.065) for i in range(9)]),
           (0,[(.04*i,-.02*i,.8-.05*i) for i in range(9)]),
           (0,[(math.cos(i*math.pi/3)*.7,math.sin(i*math.pi/3)*.7,.48) for i in range(6)])]
    for count in (2,3,5,9):
        for _ in range(6):
            cases.append((0,[(rng.uniform(-1.3,1.3),rng.uniform(-1.3,1.3),rng.uniform(.08,.7)) for _ in range(count)]))
    for _ in range(6):
        cases.append((6378137.,[(rng.uniform(-1.2,1.2),rng.uniform(-1.2,1.2),rng.uniform(.08,.5)) for _ in range(9)]))
    return cases


def physical(earth,disks,tilt):
    if earth:
        radius=4e8; phi=math.asin(earth/radius)-.3*math.asin(RS/(AU+radius))
        r=np.array([-radius*math.cos(phi),radius*math.sin(phi),0.])
    else:
        r=np.array([7e6,-2e6,3e6])
    sun=np.array([AU,0.,0.]); n=(sun-r)/np.linalg.norm(sun-r)
    u=np.cross(n,(0.,0.,1.));u/=np.linalg.norm(u);v=np.cross(n,u)
    alpha=math.asin(RS/np.linalg.norm(sun-r))
    bodies=[]
    for j,(x,y,radius) in enumerate(disks):
        # Identical apparent disks use identical physical spheres: differentiable duplicates.
        previous=disks.index((x,y,radius))
        d=4e8+previous*7e7
        separation=alpha*math.hypot(x,y)
        tangent=(x*u+y*v)/math.hypot(x,y) if x or y else u
        centre=r+d*(math.cos(separation)*n+math.sin(separation)*tangent)
        bodies.append((d*math.sin(alpha*radius),centre))
    c,s=math.cos(tilt),math.sin(tilt)
    rotation=np.array([[c,0,s],[0,1,0],[-s,0,c]])
    return rotation@r,rotation@sun,[(radius,rotation@centre) for radius,centre in bodies]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if scipy.__version__!='1.16.3':raise RuntimeError('Pinned SciPy 1.16.3 required')
    warnings.simplefilter('error',IntegrationWarning)
    rows=['# SciPy 1.16.3 QUADPACK slices; Cartesian eighth-order differences, two widths',
          '# extra_count Cr A/M EarthRadius(0=disabled) r[3] Sun[3] extra{radius,centre[3]} step_r step_s f grad_r[3] grad_s[3] a[3] Jr[9] Js[9] d_a/dK[3]']
    maximum=np.zeros(3)
    count=0
    for earth,disks in configurations():
        for tilt in (0.,.9,1.3):
            r,sun,extras=physical(earth,disks,tilt)
            bodies=([(earth,np.zeros(3))] if earth else [])+extras
            x=np.r_[r,sun]
            cr=(1.7,0.,-2.2)[count%3];ratio=.03;k=cr*ratio
            nominal,dk=values(x,bodies,k,2e-13)
            check,_=values(x,bodies,k,1e-12)
            steps=[512.,16384.]
            light_error=abs(check[0]-nominal[0]);maximum[0]=max(maximum[0],light_error)
            if light_error>2e-11:raise RuntimeError('Quadrature convergence failed')
            for attempt in range(10):
                coarse=differential(x,bodies,k,[2*v for v in steps],1e-12)
                fine=differential(x,bodies,k,steps,2e-13)
                failed=[False,False]
                convergence=[0.,0.]
                for offset in (0,3):
                    # Roundoff floor after differencing the quadrature result.
                    floor=2e-17 if offset==0 else 1e-19
                    for component in range(4):
                        scale=max(abs(fine[component,offset:offset+3]))
                        err=max(abs(fine[component,offset:offset+3]-coarse[component,offset:offset+3]))
                        bound=5e-8*scale+floor*(1 if component==0 else PRESSURE*abs(k))
                        failed[offset//3] |= err>bound
                        if component==0:convergence[offset//3]=err/(scale+floor/5e-8)
                if not any(failed):
                    maximum[1:]=np.maximum(maximum[1:],convergence)
                    break
                for j in range(2):
                    if failed[j]:steps[j]/=2
            else:
                raise RuntimeError(f'Unconverged stencil case={count}, steps={steps}')
            row=[len(extras),cr,ratio,earth,*r,*sun]
            for radius,centre in extras:row.extend((radius,*centre))
            row.extend((*steps,nominal[0],*fine[0,:3],*fine[0,3:],*nominal[1:],*fine[1:,:3].flat,*fine[1:,3:].flat,*dk))
            rows.append(' '.join(format(v,'.17g') for v in row))
            count+=1
            if count%12==0:print(f'accepted {count}',flush=True)
    args.output.write_text('\n'.join(rows)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(dict(cases=count,max_convergence=maximum.tolist(),sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())))


if __name__=='__main__':main()
