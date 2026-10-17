// Independent Cartesian variational equations integrated by Orekit 13.1.2 / Hipparchus 4.0.2.
// No production universal-variable solver or derivatives are used.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.orbits.OrbitType;
import org.orekit.propagation.SpacecraftState;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeKeplerJacobianReference {
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static double[] run(double[] initial,double dt,double mu,double step) {
        var ode=new DormandPrince853Integrator(1e-10,step,
            new double[]{1e-9,1e-9,1e-9,1e-12,1e-12,1e-12,1e-9},
            new double[]{2e-15,2e-15,2e-15,2e-15,2e-15,2e-15,2e-15});
        var p=new NumericalPropagator(ode);p.setOrbitType(OrbitType.CARTESIAN);
        var pv=new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),new Vector3D(initial[3],initial[4],initial[5]));
        p.setInitialState(new SpacecraftState(new CartesianOrbit(pv,FramesFactory.getGCRF(),EPOCH,mu)));
        var harvester=p.setupMatricesComputation("stm",null,null);
        var state=p.propagate(EPOCH.shiftedBy(dt));var end=state.getPVCoordinates();
        double[] result=new double[42];
        result[0]=end.getPosition().getX();result[1]=end.getPosition().getY();result[2]=end.getPosition().getZ();
        result[3]=end.getVelocity().getX();result[4]=end.getVelocity().getY();result[5]=end.getVelocity().getZ();
        var stm=harvester.getStateTransitionMatrix(state);
        for(int i=0;i<6;i++)for(int j=0;j<6;j++)result[6+6*i+j]=stm.getEntry(i,j);
        return result;
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);var cases=new ArrayList<double[]>();
        for(String line:Files.readAllLines(Path.of(args[0]))) {
            if(line.startsWith("#") || line.isBlank())continue;
            String[] fields=line.trim().split(" +");double[] row=new double[8];
            row[0]=Double.parseDouble(fields[1]);row[1]=Double.parseDouble(fields[2]);
            for(int k=0;k<6;k++)row[k+2]=Double.parseDouble(fields[k+3]);cases.add(row);
        }
        double mu=3.986004415e14,r=7e6,v=Math.sqrt(mu/r),period=2*Math.PI*r/v;
        for(double dt:new double[]{0,period,-period,20*period,-20*period,period/2-1e-6,period/2+1e-6,-period/2})
            cases.add(new double[]{mu,dt,r,0,0,0,v,0});
        StringBuilder out=new StringBuilder("# Orekit 13.1.2 Cartesian numerical STM, Hipparchus 4.0.2 DP853; SI, TAI/GCRF\n# case mu dt initial[6] state[6] jacobian[36 row-major]\n");
        double maxP=0,maxV=0,maxJ=0;int index=0;
        for(double[] row:cases) {
            double[] initial=java.util.Arrays.copyOfRange(row,2,8),a=run(initial,row[1],row[0],10),b=run(initial,row[1],row[0],5);
            double radius=new Vector3D(initial[0],initial[1],initial[2]).getNorm(),speed=Math.sqrt(row[0]/radius);
            for(int i=0;i<6;i++) {
                double error=Math.abs(a[i]-b[i]);if(i<3)maxP=Math.max(maxP,error);else maxV=Math.max(maxV,error);
                for(int j=0;j<6;j++)maxJ=Math.max(maxJ,Math.abs(a[6+6*i+j]-b[6+6*i+j])*(j<3?radius:speed)/(i<3?radius:speed));
            }
            out.append(index++);for(double x:row)out.append(String.format(" %.17g",x));
            for(double x:b)out.append(String.format(" %.17g",x));out.append('\n');
        }
        System.out.printf("%d cases; DP853 10/5s convergence %.17g m, %.17g m/s, %.17g dimensionless STM%n",cases.size(),maxP,maxV,maxJ);
        if(maxP>5e-4 || maxV>5e-7 || maxJ>2e-7)throw new AssertionError("Independent reference did not converge");
        Files.writeString(Path.of(args[1]),out.toString(),java.nio.charset.StandardCharsets.US_ASCII);
    }
}
