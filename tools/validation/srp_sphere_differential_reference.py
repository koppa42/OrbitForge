"""mpmath 1.3.0, 60/80-digit independent two-circle overlap and Cartesian partials.

Reads only physical inputs from the Orekit fixture, never its expected outputs.
Uses the lens-area formula and mpmath.diff, not boundary arcs or OrbitForge.
"""
import argparse
import hashlib
import json
from pathlib import Path
import mpmath as mp


def lighting(x, earth, solar):
    r = x[:3]
    s = [x[i + 3] - r[i] for i in range(3)]
    body = [-v for v in r]
    norm = lambda v: mp.sqrt(sum(z * z for z in v))
    cross = [s[1]*body[2]-s[2]*body[1], s[2]*body[0]-s[0]*body[2],
             s[0]*body[1]-s[1]*body[0]]
    separation = mp.atan2(norm(cross), sum(s[i]*body[i] for i in range(3)))
    a, b = mp.asin(solar/norm(s)), mp.asin(earth/norm(body))
    if separation >= a + b:
        return mp.mpf(1)
    if separation <= abs(a - b):
        return max(mp.mpf(0), 1 - (b/a)**2)
    d = separation
    area = (a*a*mp.acos((d*d+a*a-b*b)/(2*d*a)) +
            b*b*mp.acos((d*d+b*b-a*a)/(2*d*b)) -
            mp.sqrt((-d+a+b)*(d+a-b)*(d-a+b)*(d+a+b))/2)
    return 1 - area/(mp.pi*a*a)


def evaluate(values, precision):
    with mp.workdps(precision):
        cr, ratio, earth, solar = map(mp.mpf, values[:4])
        x = list(map(mp.mpf, values[4:10]))
        f = lambda z: lighting(z, earth, solar)
        def flux(z):
            s = [z[i+3]-z[i] for i in range(3)]
            d = mp.sqrt(sum(v*v for v in s))
            factor = -mp.mpf('4.56e-6') * mp.mpf('149597870700')**2 * f(z) / d**3
            return [factor*v for v in s]
        derivative = lambda fun,j: mp.diff(lambda v: fun(x[:j]+[v]+x[j+1:]), x[j])
        light = f(x)
        gradients = [derivative(f,j) for j in range(6)]
        parameter = flux(x)
        acceleration = [cr*ratio*v for v in parameter]
        jacobian = [cr*ratio*derivative(lambda z:flux(z)[i],j)
                    for offset in (0,3) for i in range(3) for j in range(offset,offset+3)]
        return [light,*gradients,*acceleration,*jacobian[:9],*[mp.mpf(0)]*9,
                *jacobian[9:],*parameter]


def audit(path, expected):
    rows = [list(map(float,line.split()[1:])) for line in path.read_text().splitlines()
            if line.startswith('D ')]
    maximum = dict(lighting=0., position_gradient=0., sun_gradient=0.)
    for actual,truth in zip(rows,expected,strict=True):
        maximum['lighting'] = max(maximum['lighting'], abs(actual[10]-float(truth[0])))
        for name,start in [('position_gradient',1),('sun_gradient',4)]:
            scale = max(abs(float(v)) for v in truth[start:start+3]) + 1e-30
            maximum[name] = max(maximum[name],
                max(abs(actual[10+i]-float(truth[i])) for i in range(start,start+3))/scale)
    return maximum


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--raw-orekit',type=Path)
    args=parser.parse_args()
    if mp.__version__ != '1.3.0':
        raise RuntimeError('Pinned mpmath 1.3.0 required')
    rows=['# Independent mpmath 1.3.0 lens-area formula and Cartesian derivatives, 60/80 digits',
          '# Same columns as srp_differential.txt; D cases only; inputs copied, outputs independent']
    results=[]
    maximum=mp.mpf(0)
    for line in args.input.read_text().splitlines():
        if not line.startswith('D '):
            continue
        values=list(map(float,line.split()[1:11]))
        low,high=evaluate(values,60),evaluate(values,80)
        with mp.workdps(80):
            error=max(abs(a-b) for a,b in zip(low,high,strict=True))
            maximum=max(maximum,error)
            if error>mp.mpf('1e-45'):
                raise RuntimeError('60/80-digit convergence failed')
        results.append(high)
        rows.append('D '+' '.join(format(v,'.17g') for v in [*values,*map(float,high)]))
    args.output.write_text('\n'.join(rows)+'\n',encoding='utf-8',newline='\n')
    report=dict(cases=len(results),max_60_80_difference=str(maximum),
                sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                orekit_canonical=audit(args.input,results))
    if args.raw_orekit:
        report['orekit_raw']=audit(args.raw_orekit,results)
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
