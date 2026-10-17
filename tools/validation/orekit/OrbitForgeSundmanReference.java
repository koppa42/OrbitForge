// Independent Hipparchus 4.0.2: Legendre-Gauss quadrature and DP853.
// No OrbitForge implementation or bindings are used. The transformed ODE is
// nondimensional; a second DP853 integration in physical time checks the orbit.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.analysis.integration.IterativeLegendreGaussIntegrator;
import org.hipparchus.dfp.Dfp;
import org.hipparchus.dfp.DfpField;
import org.hipparchus.ode.FieldOrdinaryDifferentialEquation;
import org.hipparchus.ode.FieldExpandableODE;
import org.hipparchus.ode.FieldODEState;
import org.hipparchus.ode.nonstiff.DormandPrince853FieldIntegrator;

public final class OrbitForgeSundmanReference {
    static final double MU = 3.986004415e14, R = 6378136.3;
    static double norm(double[] v, int begin) {
        return Math.sqrt(v[begin]*v[begin]+v[begin+1]*v[begin+1]+v[begin+2]*v[begin+2]);
    }
    static double period(double e, double n) {
        // Integrate eccentric anomaly for every exponent. This differs from the
        // production Simpson / true-anomaly quadrature, including n=1 and n=2.
        var quadrature = new IterativeLegendreGaussIntegrator(16, 1e-14, 1e-14, 3, 64);
        return 2*quadrature.integrate(1000000,
            angle -> Math.pow((1-e)+2*e*Math.pow(Math.sin(angle/2),2),1-n),0,Math.PI);
    }
    static double[] initial(double e) {
        double r=7000000, v=Math.sqrt(MU*(1+e)/r), angle=.4, inclination=.6;
        return new double[]{r*Math.cos(angle),r*Math.sin(angle)*Math.cos(inclination),
            r*Math.sin(angle)*Math.sin(inclination), -v*Math.sin(angle),
            v*Math.cos(angle)*Math.cos(inclination), v*Math.cos(angle)*Math.sin(inclination)};
    }
    static Dfp norm(Dfp[] v, int begin) {
        return v[begin].multiply(v[begin]).add(v[begin+1].multiply(v[begin+1]))
            .add(v[begin+2].multiply(v[begin+2])).sqrt();
    }
    static Dfp[] force(int kind, Dfp seconds, Dfp[] s) {
        Dfp r=norm(s,0), mu=r.newInstance(MU), k=mu.negate().divide(r.pow(3));
        Dfp[] result={k.multiply(s[0]),k.multiply(s[1]),k.multiply(s[2])};
        if(kind==1) {
            Dfp j2=r.newInstance(5).sqrt().multiply(.484165371736e-3);
            Dfp z2=s[2].divide(r).pow(2), factor=j2.multiply(1.5).multiply(mu)
                .multiply(R).multiply(R).divide(r.pow(5));
            for(int i=0;i<3;i++)result[i]=result[i].add(factor.multiply(s[i])
                .multiply(z2.multiply(5).subtract(i==2?3:1)))
                .add(seconds.divide(1900).add(.7*i).sin().multiply(1e-7))
                .subtract(s[i+3].multiply(1e-12));
        }
        return result;
    }
    static final DfpField FIELD = new DfpField(32);
    static Dfp[] solve(int kind, double e, double n, Dfp end, double maximum,
                       boolean regularized) {
        Dfp a=FIELD.newDfp(7000000/(1-e)), scale=a.divide(FIELD.newDfp(MU).divide(a).sqrt());
        double[] physical=initial(e);
        Dfp[] first=new Dfp[7];
        for(int i=0;i<3;i++) {
            first[i]=FIELD.newDfp(physical[i]).divide(a);
            first[i+3]=FIELD.newDfp(physical[i+3]).multiply(scale).divide(a);
        }
        first[6]=FIELD.getZero();
        var ode=new FieldOrdinaryDifferentialEquation<Dfp>() {
            public int getDimension() {return 7;}
            public Dfp[] computeDerivatives(Dfp independent, Dfp[] y) {
                Dfp time=regularized?y[6]:independent;
                Dfp[] s=new Dfp[6], result=new Dfp[7];
                for(int i=0;i<3;i++){s[i]=y[i].multiply(a);s[i+3]=y[i+3].multiply(a).divide(scale);}
                Dfp[] acceleration=force(kind,time.multiply(scale),s);
                Dfp r=norm(y,0);
                Dfp g=!regularized?FIELD.getOne():n==1?r:n==2?r.multiply(r):r.multiply(r.sqrt());
                for(int i=0;i<3;i++){
                    result[i]=y[i+3].multiply(g);
                    result[i+3]=acceleration[i].multiply(scale).multiply(scale).divide(a).multiply(g);
                }
                result[6]=g;
                return result;
            }
        };
        var integrator=new DormandPrince853FieldIntegrator<Dfp>(FIELD,1e-16,maximum,1e-20,1e-18);
        Dfp target=regularized?end:end.divide(scale);
        Dfp[] result=integrator.integrate(new FieldExpandableODE<Dfp>(ode),
            new FieldODEState<Dfp>(FIELD.getZero(),first),target).getPrimaryState();
        for(int i=0;i<3;i++){result[i]=result[i].multiply(a);result[i+3]=result[i+3].multiply(a).divide(scale);}
        result[6]=result[6].multiply(scale);
        return result;
    }
    static double difference(Dfp[] a,Dfp[] b,int begin) {
        Dfp[] delta={a[begin].subtract(b[begin]),a[begin+1].subtract(b[begin+1]),
                     a[begin+2].subtract(b[begin+2])};
        return norm(delta,0).getReal();
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        var out=new StringBuilder("# Hipparchus 4.0.2 Legendre-Gauss / Dfp(32) DP853; mu=3.986004415e14\n"+
            "# P eccentricity exponent normalized_q_period\n"+
            "# T kind eccentricity exponent q_end initial_r[3] initial_v[3] final_r[3] final_v[3] physical_seconds\n");
        for(double e:new double[]{0,.7,.98,.9999})
            for(double n:new double[]{.1,.5,1,1.5,2,2.5,3,5,10})
                out.append(String.format("P %.17g %.17g %.17g%n",e,n,period(e,n)));
        double maxR=0,maxV=0,maxT=0,crossR=0,crossV=0;
        int count=0;
        for(int kind=0;kind<2;kind++)
            for(double e:new double[]{0,.7,.98})
                for(double n:new double[]{1,1.5,2})
                    for(double turns:new double[]{-.375,.4375,1.125}) {
                        System.out.printf("Starting kind=%d e=%g n=%g turns=%g%n",kind,e,n,turns);
                        double p=period(e,n), q=turns*p;
                        Dfp[] coarse=solve(kind,e,n,FIELD.newDfp(q),p/256,true);
                        Dfp[] fine=solve(kind,e,n,FIELD.newDfp(q),p/512,true);
                        maxR=Math.max(maxR,difference(coarse,fine,0));
                        maxV=Math.max(maxV,difference(coarse,fine,3));
                        maxT=Math.max(maxT,coarse[6].subtract(fine[6]).abs().getReal());
                        Dfp[] physical=solve(kind,e,n,fine[6],2*Math.PI/2048,false);
                        if(difference(fine,physical,0)>crossR)
                            System.out.printf("Normal-time max case kind=%d e=%g n=%g turns=%g r=%.17g%n",kind,e,n,turns,difference(fine,physical,0));
                        crossR=Math.max(crossR,difference(fine,physical,0));
                        crossV=Math.max(crossV,difference(fine,physical,3));
                        out.append(String.format("T %d %.17g %.17g %.17g",kind,e,n,q));
                        for(double v:initial(e))out.append(String.format(" %.17g",v));
                        for(Dfp v:fine)out.append(String.format(" %.17g",v.getReal()));
                        out.append('\n');count++;
                    }
        System.out.printf("%d trajectories; q ceiling halving r=%.17g m v=%.17g m/s t=%.17g s%n",count,maxR,maxV,maxT);
        System.out.printf("Independent normal-time integration r=%.17g m v=%.17g m/s%n",crossR,crossV);
        // Reference budgets: <=1% of the production 5 mm / 5e-6 m/s / 0.5 us
        // checks. The normalized-q endpoint convergence is checked separately.
        if(maxR>1e-6 || maxV>1e-9 || maxT>5e-9 || crossR>5e-5 || crossV>5e-8)
            throw new AssertionError("Reference convergence or change of independent variable failed");
        Files.writeString(Path.of(args[0]),out.toString().replace("\r\n","\n"),java.nio.charset.StandardCharsets.US_ASCII);
    }
}
