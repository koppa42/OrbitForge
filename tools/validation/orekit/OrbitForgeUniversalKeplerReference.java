// Independent reference for the VOP two-body flow. Orekit 13.1.2,
// Hipparchus 4.0.2 DP853; no production Stumpff solver is used.
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

public final class OrbitForgeUniversalKeplerReference {
    static final double MU = 3.986004415e14;
    static final AbsoluteDate EPOCH = new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static double[] run(double[] initial, double dt, double step) {
        var ode = new DormandPrince853Integrator(1e-10,step,
            new double[]{1e-9,1e-9,1e-9,1e-12,1e-12,1e-12,1e-9},
            new double[]{2e-15,2e-15,2e-15,2e-15,2e-15,2e-15,2e-15});
        var propagator = new NumericalPropagator(ode);
        propagator.setOrbitType(OrbitType.CARTESIAN);
        var pv = new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
                                    new Vector3D(initial[3],initial[4],initial[5]));
        propagator.setInitialState(new SpacecraftState(
            new CartesianOrbit(pv,FramesFactory.getGCRF(),EPOCH,MU)));
        var result = propagator.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
        return new double[]{result.getPosition().getX(),result.getPosition().getY(),result.getPosition().getZ(),
                            result.getVelocity().getX(),result.getVelocity().getY(),result.getVelocity().getZ()};
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.print("# Orekit 13.1.2 / Hipparchus 4.0.2 DP853; pure Newtonian, TAI/GCRF, SI\n");
        System.out.print("# case mu dt initial[6] expected[6]; max-step 10/5 s convergence\n");
        double maxP=0,maxV=0;
        for(int index=0;index<10;index++) {
            double e = new double[]{0,.01,.7,.95,.999999,1,1.000001,1.4,0,0}[index];
            double r = index==8 ? 42164000 : 7e6;
            double speed = Math.sqrt(MU/r*(1+e));
            // Orthonormal rotation: polar/inclined/equatorial cases, no element conversion.
            double angle = index*.21, tilt = index*.19;
            double c=Math.cos(angle),s=Math.sin(angle),ct=Math.cos(tilt),st=Math.sin(tilt);
            double[] initial={r*c,r*s*ct,r*s*st,-speed*s,speed*c*ct,speed*c*st};
            if(index==9) initial=new double[]{7e6,-1.2e6,1e6,900,6500,4000};
            for(double dt:new double[]{-86400,-6000,-123.456789,-1e-9,1e-9,123.456789,6000,86400}) {
                double[] coarse=run(initial,dt,10),fine=run(initial,dt,5);
                for(int axis=0;axis<6;axis++) {
                    double error=Math.abs(coarse[axis]-fine[axis]);
                    if(axis<3) maxP=Math.max(maxP,error); else maxV=Math.max(maxV,error);
                }
                System.out.printf("%d %.17g %.17g",index,MU,dt);
                for(double value:initial) System.out.printf(" %.17g",value);
                for(double value:fine) System.out.printf(" %.17g",value);
                System.out.print("\n");
            }
        }
        System.err.printf("DP853 max-step 10/5 s convergence: position %.17g m; velocity %.17g m/s%n",maxP,maxV);
        if(maxP>2e-4 || maxV>2e-7) throw new AssertionError("Reference convergence failed");
    }
}
