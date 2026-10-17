// Independent long-period references: Hipparchus 4.0.2 DP853, SI / fixed axes / TAI.
// Compile with OrbitForgeBsReference.java; no OrbitForge production code is called.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.ode.OrdinaryDifferentialEquation;
import org.hipparchus.ode.ODEState;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;

public final class OrbitForgeLongStepReference {
    // Nondimensional coordinates balance the position/velocity error scales.
    // The independent force is central + J2, evaluated without production code.
    static double[] solve(int kind, double[] initial, double end, double ceiling, double length) {
        double time = Math.sqrt(length*length*length/OrbitForgeBsReference.MU);
        double speed = length/time;
        double[] y0 = initial.clone();
        for (int i=0;i<3;i++) { y0[i] /= length; y0[i+3] /= speed; }
        OrdinaryDifferentialEquation ode = new OrdinaryDifferentialEquation() {
            public int getDimension() { return 6; }
            public double[] computeDerivatives(double t, double[] y) {
                double r = OrbitForgeBsReference.norm(y[0],y[1],y[2]);
                double[] f = {y[3],y[4],y[5],-y[0]/(r*r*r),-y[1]/(r*r*r),-y[2]/(r*r*r)};
                if (kind != 0) {
                    double radius = OrbitForgeBsReference.R/length;
                    double k = 1.5*Math.sqrt(5)*.484165371736e-3*radius*radius/Math.pow(r,5);
                    double z2 = y[2]*y[2]/(r*r);
                    for (int i=0;i<3;i++) f[i+3] += k*y[i]*(5*z2-(i==2?3:1));
                }
                return f;
            }
        };
        var integrator = new DormandPrince853Integrator(1e-10/time,ceiling/time,1e-16,1e-15);
        double[] state = integrator.integrate(ode,new ODEState(0,y0),end/time).getPrimaryState();
        for (int i=0;i<3;i++) { state[i] *= length; state[i+3] *= speed; }
        return state;
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        StringBuilder out = new StringBuilder("# Hipparchus 4.0.2 DP853; SI, fixed axes, TAI 0\n# kind elapsed_seconds initial_r[3] initial_v[3] propagated_r[3] propagated_v[3]\n");
        double maxR = 0, maxV = 0;
        int count = 0;
        for (double a : new double[]{42164000, 2e8, 1e9, 1e10}) {
            for (double e : new double[]{0, .7}) {
                double radius = a * (1-e), speed = Math.sqrt(OrbitForgeBsReference.MU * (1+e) / radius);
                double[] initial = {radius,0,0,0,speed*Math.cos(.4),speed*Math.sin(.4)};
                double[] times = a == 1e10 ? new double[]{86400,259200.123456789,-259200.123456789,1382400}
                                          : new double[]{86400,259200.123456789,-259200.123456789};
                for (int kind : new int[]{0,1}) {
                    for (double end : times) {
                        double ceiling = a >= 1e9 ? 21600 : 600;
                        double[] coarse = solve(kind,initial,end,ceiling,a);
                        double[] fine = solve(kind,initial,end,ceiling/2,a);
                        if (OrbitForgeBsReference.norm(coarse[0]-fine[0],coarse[1]-fine[1],coarse[2]-fine[2]) > 1e-4)
                            System.out.printf("reference refinement exceeds bound: a=%.1f e=%.1f kind=%d t=%.12f%n",a,e,kind,end);
                        maxR = Math.max(maxR,OrbitForgeBsReference.norm(coarse[0]-fine[0],coarse[1]-fine[1],coarse[2]-fine[2]));
                        maxV = Math.max(maxV,OrbitForgeBsReference.norm(coarse[3]-fine[3],coarse[4]-fine[4],coarse[5]-fine[5]));
                        out.append(kind).append(' ').append(String.format("%.17g",end));
                        for (double v : initial) out.append(String.format(" %.17g",v));
                        for (double v : fine) out.append(String.format(" %.17g",v));
                        out.append('\n'); count++;
                    }
                }
            }
        }
        if (count != 52 || maxR > 1e-4 || maxV > 1e-8)
            throw new AssertionError("Reference convergence: " + count + " / " + maxR + " / " + maxV);
        Files.writeString(Path.of(args[0]),out.toString(),java.nio.charset.StandardCharsets.US_ASCII);
        System.out.printf("%d rows; max refinement %.17g m / %.17g m/s%n",count,maxR,maxV);
    }
}
