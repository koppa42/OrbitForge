// Independent reference: Hipparchus 4.0.2 Gragg-Bulirsch-Stoer, no OrbitForge code.
// Reuses the six physical benchmark initial states from rkv89.txt; its expected
// outputs are ignored. Compare 60/30 s ceilings before writing the fine result.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.ode.OrdinaryDifferentialEquation;
import org.hipparchus.ode.ODEState;
import org.hipparchus.ode.nonstiff.GraggBulirschStoerIntegrator;

public final class OrbitForgeBsReference {
    static final double MU = 3.986004415e14, R = 6378136.3;
    static double norm(double x, double y, double z) { return Math.sqrt(x*x+y*y+z*z); }
    static double[] solve(int kind, double[] initial, double end, double ceiling) {
        OrdinaryDifferentialEquation ode = new OrdinaryDifferentialEquation() {
            public int getDimension() { return 6; }
            public double[] computeDerivatives(double t, double[] y) {
                double r = norm(y[0],y[1],y[2]);
                double[] a = {y[3],y[4],y[5], -MU*y[0]/(r*r*r), -MU*y[1]/(r*r*r), -MU*y[2]/(r*r*r)};
                if (kind != 0) {
                    double j2 = Math.sqrt(5)*.484165371736e-3;
                    double z2 = y[2]*y[2]/(r*r), k = 1.5*j2*MU*R*R/Math.pow(r,5);
                    for (int i=0;i<3;i++) a[i+3] += k*y[i]*(5*z2-(i==2?3:1));
                }
                if (kind == 2) {
                    double angle = t*2*Math.PI/(27.321661*86400)+.4;
                    double[] b = {384400000*Math.cos(angle),384400000*Math.sin(angle)*Math.cos(.3),384400000*Math.sin(angle)*Math.sin(.3)};
                    double nr = norm(b[0],b[1],b[2]), nd = norm(b[0]-y[0],b[1]-y[1],b[2]-y[2]);
                    for (int i=0;i<3;i++) a[i+3] += 4.902800066e12*((b[i]-y[i])/(nd*nd*nd)-b[i]/(nr*nr*nr));
                }
                return a;
            }
        };
        var integrator = new GraggBulirschStoerIntegrator(1e-10,ceiling,
            new double[]{1e-9,1e-9,1e-9,1e-12,1e-12,1e-12},
            new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
        // Endpoint integration; no dense-output interpolation is used for comparison.
        integrator.setInterpolationControl(false,4);
        return integrator.integrate(ode,new ODEState(0,initial),end).getPrimaryState();
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        byte[] bytes=Files.readAllBytes(Path.of(args[0]));
        String sha=java.util.HexFormat.of().formatHex(java.security.MessageDigest.getInstance("SHA-256").digest(bytes));
        if (!sha.equals("da35cab3683d41c18b02f46585c42812836bc66637b8797c6554c1cb8ae4f985"))
            throw new IllegalArgumentException("Wrong benchmark inputs");
        StringBuilder out=new StringBuilder("# Hipparchus 4.0.2 Gragg-Bulirsch-Stoer; SI, fixed axes, TAI 0\n# kind elapsed_seconds initial_r[3] initial_v[3] propagated_r[3] propagated_v[3]\n");
        double maxR=0,maxV=0;int count=0;
        for (String line:new String(bytes,java.nio.charset.StandardCharsets.US_ASCII).split("\\R")) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] fields=line.strip().split("\\s+");
            int kind=Integer.parseInt(fields[0]);double end=Double.parseDouble(fields[1]);
            double[] initial=new double[6];for(int i=0;i<6;i++)initial[i]=Double.parseDouble(fields[i+2]);
            double[] coarse=solve(kind,initial,end,60),fine=solve(kind,initial,end,30);
            maxR=Math.max(maxR,norm(coarse[0]-fine[0],coarse[1]-fine[1],coarse[2]-fine[2]));
            maxV=Math.max(maxV,norm(coarse[3]-fine[3],coarse[4]-fine[4],coarse[5]-fine[5]));
            out.append(kind).append(' ').append(String.format("%.17g",end));
            for(double v:initial)out.append(String.format(" %.17g",v));
            for(double v:fine)out.append(String.format(" %.17g",v));
            out.append('\n');count++;
        }
        if(maxR>5e-5 || maxV>5e-8)throw new AssertionError("Reference convergence "+maxR+" / "+maxV);
        Files.writeString(Path.of(args[1]),out.toString(),java.nio.charset.StandardCharsets.US_ASCII);
        System.out.printf("%d rows; 60/30 s max position %.17g m, velocity %.17g m/s%n",count,maxR,maxV);
    }
}
