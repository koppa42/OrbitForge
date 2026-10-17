"""Cartesian 8-point density gradients from unmodified GMAT JR / author NRLMSISE.

Pass the streaming adapter executable and output path. The two derivative steps
must agree before a row is written. No OrbitForge imports, headers or binaries.
"""
import argparse
import math
from pathlib import Path
import subprocess

A = 6378137.0
F = 1 / 298.257223563
E2 = F * (2 - F)
B = A * (1 - F)
SUN = (1.4e11, -5e10, 3e10)


def position(lat, lon, h):
    n = A / math.sqrt(1 - E2 * math.sin(lat)**2)
    return ((n+h)*math.cos(lat)*math.cos(lon), (n+h)*math.cos(lat)*math.sin(lon),
            (n*(1-E2)+h)*math.sin(lat))


def coordinates(r, approximate):
    # Independent normal-footpoint iteration, with altitude from the normal plane.
    x, y, z = r
    p = math.hypot(x, y)
    lat = math.atan2(z, p)
    for _ in range(20):
        n = A / math.sqrt(1-E2*math.sin(lat)**2)
        lat = math.atan2(z+E2*n*math.sin(lat), p)
    n = A / math.sqrt(1-E2*math.sin(lat)**2)
    h = p/math.cos(lat) - n
    if approximate:
        radius = math.hypot(*r)
        h = radius - B/math.sqrt(1-E2*(p/radius)**2)
    return lat, math.atan2(y,x), h


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adapter', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    requests, evaluations, rows = [], [], []

    def request(model, lat, lon, height, mode):
        hour = math.remainder(lon-math.atan2(SUN[1], SUN[0]), 2*math.pi)
        decl = math.atan2(SUN[2], math.hypot(SUN[0], SUN[1]))
        weather = (150,150,3,15,0) if mode == 0 else ((180,165,5.5,37.25,0) if model == 0 else (210,160,3,7,1))
        index = len(requests)
        requests.append(' '.join(format(x,'.17g') for x in (model,height,lat,lon,hour,decl,*weather)))
        return index

    def evaluate(r, approximate, mode):
        lat, lon, h = coordinates(r, approximate)
        if mode == 2 and h < 120000:
            lo = request(1,lat,lon,h,mode)
            if h <= 90000:
                indices = (lo,lo,0)
            else:
                t = (h-90000)/30000
                indices = (lo,request(0,lat,lon,h,mode),t*t*(3-2*t))
        else:
            jr = request(0,lat,lon,h,mode)
            indices = (jr,jr,0)
        evaluations.append(indices)
        return len(evaluations)-1

    for mode in range(3):
        for approximate in range(2):
            for height in (95000,105000,115000):
                for lat, lon in ((-80,.4),(0,.7),(45,-2),(80,2.9)):
                    r = position(math.radians(lat),lon,height)
                    nominal = evaluate(r,approximate,mode)
                    samples = []
                    steps = (64.,128.)
                    for step in steps:
                        axes = []
                        for axis in range(3):
                            values = []
                            for k in range(1,5):
                                pair = []
                                for sign in (1,-1):
                                    point = list(r)
                                    point[axis] += sign*k*step
                                    pair.append(evaluate(point,approximate,mode))
                                values.append(pair)
                            axes.append(values)
                        samples.append(axes)
                    rows.append((mode,approximate,r,nominal,samples,steps))
    response = subprocess.run([str(args.adapter.resolve())],input='\n'.join(requests)+'\n',
                              encoding='utf-8',capture_output=True,check=True)
    densities = [float(x) for x in response.stdout.splitlines()]
    assert len(densities) == len(requests)
    assert all(math.isfinite(x) and x > 0 for x in densities)
    values = [densities[i]+w*(densities[j]-densities[i]) for i,j,w in evaluations]
    out = ['# GMAT c9b78bd JR + author NRLMSISE bc9a2fe; Cartesian eighth-order differences',
           '# UTC 2000-02-29 13:23:12.5; Sun=(1.4e11,-5e10,3e10); two Cartesian stencil sizes agree',
           '# mode approx r[3] rho grad_rho[3]']
    max_difference = 0
    for mode,approximate,r,nominal,samples,steps in rows:
        gradients = []
        for step, axes in zip(steps,samples):
            gradients.append([sum(w*(values[p]-values[m]) for w,(p,m) in
                             zip((4/5,-1/5,4/105,-1/280),axis))/step for axis in axes])
        scale = max(abs(x) for x in gradients[0])
        difference = max(abs(x-y) for x,y in zip(*gradients))/scale
        error = max(abs(x-y) for x,y in zip(*gradients))
        # Double GMAT low-altitude root/polynomial noise is amplified by differencing.
        assert error < 2e-7*scale + 2e-12*values[nominal], (mode,approximate,r,error)
        max_difference = max(max_difference,difference)
        out.append(' '.join(format(x,'.17g') for x in (mode,approximate,*r,values[nominal],*gradients[0])))
    args.output.write_text('\n'.join(out)+'\n',encoding='utf-8',newline='\n')
    print(f'{len(rows)} rows; maximum step-convergence relative difference {max_difference:.9g}')


if __name__ == '__main__':
    main()
