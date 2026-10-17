// Independent Orekit 13.1.2 / Hipparchus 4.0.2 reference for OrbitForge's documented
// VOP interpolation recipe. Not a reference exported by STK. No OrbitForge code.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.analysis.polynomials.PolynomialFunctionLagrangeForm;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.J2OnlyPerturbation;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.orbits.OrbitType;
import org.orekit.propagation.SpacecraftState;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeVopInterpolationReference {
    static final double MU=3.986004415e14;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static double[] values(PVCoordinates pv) {
        return new double[]{pv.getPosition().getX(),pv.getPosition().getY(),pv.getPosition().getZ(),
                            pv.getVelocity().getX(),pv.getVelocity().getY(),pv.getVelocity().getZ()};
    }
    static CartesianOrbit orbit(double[] y,double mu) {
        return new CartesianOrbit(new PVCoordinates(new Vector3D(y[0],y[1],y[2]),
            new Vector3D(y[3],y[4],y[5])),FramesFactory.getGCRF(),EPOCH,mu);
    }
    static double[] kepler(double[] y,double dt,double mu) {
        return values(new KeplerianPropagator(orbit(y,mu)).propagate(EPOCH.shiftedBy(dt)).getPVCoordinates());
    }
    static double[] numerical(double[] y,double dt,double step) {
        var ode=new DormandPrince853Integrator(1e-10,step,
            new double[]{1e-9,1e-9,1e-9,1e-12,1e-12,1e-12,1e-9},
            new double[]{2e-15,2e-15,2e-15,2e-15,2e-15,2e-15,2e-15});
        var p=new NumericalPropagator(ode);p.setOrbitType(OrbitType.CARTESIAN);
        p.setInitialState(new SpacecraftState(orbit(y,MU)));
        p.addForceModel(new J2OnlyPerturbation(MU,6378136.3,1.0826266835531513e-3,FramesFactory.getGCRF()));
        return values(p.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates());
    }
    static void numbers(StringBuilder out,double[] y) {
        for(double v:y) out.append(String.format(" %.17g",v));out.append('\n');
    }
    static double[] interpolate(long[] times,double[][] nodes,long query,int order,double mu) {
        for(int i=0;i<times.length;i++) if(query==times[i]) return nodes[i].clone();
        if(order==0) {
            int i=0;while(i+1<times.length && times[i+1]<query)i++;
            return kepler(nodes[i],(query-times[i])*1e-9,mu);
        }
        int hi=1;while(times[hi]<query)hi++;
        int lo=hi-1;hi++;
        while(hi-lo<order+1) {
            if(lo>0 && (hi==times.length || query-times[lo-1]<=times[hi]-query))lo--;else hi++;
        }
        int count=hi-lo;double[] x=new double[count];double[][] y=new double[count][];
        for(int i=0;i<count;i++) {
            x[i]=(times[i+lo]-query)*1e-9;
            y[i]=kepler(nodes[i+lo],-x[i],mu);
        }
        double[] result=new double[6];
        for(int axis=0;axis<6;axis++) {
            double[] residual=new double[count];
            for(int i=0;i<count;i++)residual[i]=y[i][axis]-y[count/2][axis];
            // Hipparchus Neville interpolation, independent of production barycentric sums.
            result[axis]=y[count/2][axis]+PolynomialFunctionLagrangeForm.evaluate(x,residual,0);
        }
        return result;
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        StringBuilder out=new StringBuilder("# Orekit 13.1.2 Keplerian / DP853 J2; Hipparchus 4.0.2 Neville; SI, TAI relative ns, GCRF\n# C kind order step mu count; N ns state[6]; Q ns interpolated[6] truth[6]\n");
        int cases=0,queries=0;double maxP=0,maxV=0;
        for(int kind=0;kind<5;kind++) for(int order=0;order<=29;order++) {
            // All orders, conics, nonuniform nodes, and an intentionally different VOP GM.
            double e=new double[]{0,.7,1.4,.1,.1}[kind];
            double speed=Math.sqrt(MU/7e6*(1+e));
            double[] initial={7e6,0,0,0,speed*Math.cos(.6),speed*Math.sin(.6)};
            double mu=kind==4?MU*1.01:MU;
            int count=Math.max(order+1,4);long[] times=new long[count];double[][] nodes=new double[count][];
            out.append(String.format("C %d %d %d %.17g %d\n",kind,order,120,mu,count));
            for(int i=0;i<count;i++) {
                times[i]=(i*120L+((i%2==1)?17:0)-count*60L)*1_000_000_000L;
                nodes[i]=kind<3?kepler(initial,times[i]*1e-9,MU):numerical(initial,times[i]*1e-9,5);
                out.append("N ").append(times[i]);numbers(out,nodes[i]);
            }
            long span=times[count-1]-times[0];
            for(long ns:new long[]{times[0],times[0]+1,times[0]+span*123/1000,
                times[0]+span/2+1234567,times[0]+span*789/1000,times[count-1]-1,times[count-1]}) {
                double[] result=interpolate(times,nodes,ns,order,mu);
                double[] truth=kind<3?kepler(initial,ns*1e-9,MU):numerical(initial,ns*1e-9,5);
                out.append("Q ").append(ns);
                for(double v:result)out.append(String.format(" %.17g",v));numbers(out,truth);queries++;
            }
            cases++;
        }
        // Actual J2 trajectory error at fixed queries, h and h/2; not merely recipe agreement.
        for(int h:new int[]{600,300,150}) {
            int count=17;long[] times=new long[count];double[][] nodes=new double[count][];
            double[] initial={7e6,0,0,0,6800,3000};
            out.append(String.format("C %d %d %d %.17g %d\n",5,7,h,MU,count));
            for(int i=0;i<count;i++) {
                times[i]=(i-8L)*h*1_000_000_000L;
                nodes[i]=numerical(initial,times[i]*1e-9,5);
                out.append("N ").append(times[i]);numbers(out,nodes[i]);
            }
            for(long ns:new long[]{-107_123_456_789L,17_123_456_789L,283_123_456_789L}) {
                double[] result=interpolate(times,nodes,ns,7,MU);
                double[] truth=numerical(initial,ns*1e-9,5),coarse=numerical(initial,ns*1e-9,10);
                for(int a=0;a<6;a++){double err=Math.abs(truth[a]-coarse[a]);if(a<3)maxP=Math.max(maxP,err);else maxV=Math.max(maxV,err);}
                out.append("Q ").append(ns);
                for(double v:result)out.append(String.format(" %.17g",v));numbers(out,truth);queries++;
            }
            cases++;
        }
        if(maxP>1e-6 || maxV>1e-9)throw new AssertionError("J2 reference convergence failed");
        Files.writeString(Path.of(args[0]),out.toString(),java.nio.charset.StandardCharsets.US_ASCII);
        System.out.printf("%d cases, %d queries; J2 DP853 10/5s convergence %.17g m %.17g m/s%n",cases,queries,maxP,maxV);
    }
}
