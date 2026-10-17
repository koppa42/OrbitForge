// Independent Hipparchus 4.0.2 interpolation; no OrbitForge code or bindings.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.analysis.differentiation.UnivariateDerivative1;
import org.hipparchus.analysis.interpolation.HermiteInterpolator;
import org.hipparchus.analysis.polynomials.PolynomialFunctionLagrangeForm;

public final class OrbitForgeInterpolationReference {
    static final double MU = 3.986004415e14, R = 7e6, W = Math.sqrt(MU / (R*R*R));
    static double[] state(double t, int kind) {
        double phase = W * t + .27, c = Math.cos(.4), s = Math.sin(.4);
        double[] y = {R*Math.cos(phase),R*c*Math.sin(phase),R*s*Math.sin(phase),
                      -R*W*Math.sin(phase),R*W*c*Math.cos(phase),R*W*s*Math.cos(phase)};
        // Manufactured independent velocity data: Lagrange must not differentiate position.
        if (kind == 2) { y[3] += 300 + .07*t*t; y[4] -= 21*Math.sin(.003*t); }
        return y;
    }
    static void numbers(StringBuilder out, double[] values) {
        for (double v : values) out.append(String.format(" %.17g",v));
        out.append('\n');
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        StringBuilder out = new StringBuilder("# Hipparchus 4.0.2 interpolation; SI, TAI-relative ns, fixed axes\n# C kind method(0=Lagrange,1=Hermite) order count; N ns r[3] v[3]; Q ns r[3] v[3]\n");
        int cases=0, queries=0;
        for (int method=0;method<2;method++) for(int order=0;order<=29;order++) {
            if (method==1 && (order<3 || order%2==0)) continue;
            for (int kind=0;kind<(method==0?3:2);kind++) {
                int n=method==0?order+1:(order+1)/2;
                // order zero has several samples; its contract is left-held, not nearest-node.
                int count=order==0?4:n;
                long[] times=new long[count]; double[] x=new double[count]; double[][] nodes=new double[count][];
                out.append(String.format("C %d %d %d %d\n",kind,method,order,count));
                for (int i=0;i<count;i++) {
                    times[i]=-300_000_000_000L+i*20_000_000_000L+(kind==1 && i%2==1?3_000_000_000L:0);
                    x[i]=times[i]*1e-9;nodes[i]=state(x[i],kind);
                    out.append("N ").append(times[i]);numbers(out,nodes[i]);
                }
                long span=times[count-1]-times[0];
                long[] query={times[0],times[0]+1,times[0]+span*123/1000,
                              times[0]+span/2+1234567,times[0]+span*789/1000,
                              times[count-1]-1,times[count-1]};
                HermiteInterpolator h=new HermiteInterpolator();
                if(method==1) for(int i=0;i<count;i++)
                    h.addSamplePoint(x[i],new double[]{nodes[i][0],nodes[i][1],nodes[i][2]},
                                          new double[]{nodes[i][3],nodes[i][4],nodes[i][5]});
                for(long ns:query) {
                    double t=ns*1e-9;double[] result=new double[6];boolean exact=false;
                    for(int i=0;i<count;i++) if(times[i]==ns) {result=nodes[i].clone();exact=true;break;}
                    if(!exact) {
                        if(order==0) {
                            int index=0;while(index+1<count && times[index+1]<ns) index++;
                            result=nodes[index].clone();
                        } else if(method==0) {
                            for(int axis=0;axis<6;axis++) {
                                double[] values=new double[n];for(int i=0;i<n;i++) values[i]=nodes[i][axis];
                                result[axis]=PolynomialFunctionLagrangeForm.evaluate(x,values,t);
                            }
                        } else {
                            UnivariateDerivative1[] y=h.value(new UnivariateDerivative1(t,1));
                            for(int axis=0;axis<3;axis++) {result[axis]=y[axis].getValue();result[axis+3]=y[axis].getFirstDerivative();}
                        }
                    }
                    out.append("Q ").append(ns);numbers(out,result);queries++;
                }
                cases++;
            }
        }
        Files.writeString(Path.of(args[0]),out.toString(),java.nio.charset.StandardCharsets.US_ASCII);
        System.out.printf("%d cases, %d queries%n",cases,queries);
    }
}
