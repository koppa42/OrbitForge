// Independent reference: Orekit 13.1.2 ThirdBodyAttraction + Hipparchus 4.0.2 DP853.
// Synthetic moving geocentric bodies isolate force evaluation and integration from SPICE.
import java.util.Locale;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.FieldVector3D;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.ThirdBodyAttraction;
import org.orekit.frames.Frame;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.orbits.OrbitType;
import org.orekit.propagation.SpacecraftState;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.FieldAbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.ExtendedPositionProvider;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeThirdBodyReference {
    static final double MU=3.986004415e14, SUN=1.3271244004127942e20, MOON=4.9028001184575496e12;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static ExtendedPositionProvider body(boolean sun) {
        final double radius=sun ? 1.496e11 : 384400000;
        final double rate=2*Math.PI/(sun ? 365.25*86400 : 27.321661*86400);
        final double phase=sun ? .3 : 1.2, tilt=sun ? .409 : .5;
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date, Frame frame) {
                double angle=phase+rate*date.durationFrom(EPOCH);
                return new Vector3D(radius*Math.cos(angle), radius*Math.sin(angle)*Math.cos(tilt),
                                    radius*Math.sin(angle)*Math.sin(tilt));
            }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date, Frame frame) {
                T angle=date.durationFrom(EPOCH).multiply(rate).add(phase);
                return new FieldVector3D<>(angle.cos().multiply(radius),
                    angle.sin().multiply(radius*Math.cos(tilt)), angle.sin().multiply(radius*Math.sin(tilt)));
            }
        };
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2; moving synthetic Sun/Moon; point Earth; TAI / GCRF");
        System.out.println("# mode dt rx ry rz vx vy vz expected_r expected_v third_body_acceleration_at_initial");
        double[][] cases={{7e6,0,0,0,6200,5300},{42164000,0,0,0,Math.sqrt(MU/42164000),0}};
        for(int mode=0;mode<4;mode++) {
            double sunMu=mode==3 ? SUN*1.01 : SUN, moonMu=mode==3 ? MOON*.99 : MOON;
            var sun=new ThirdBodyAttraction(body(true),"synthetic-sun",sunMu);
            var moon=new ThirdBodyAttraction(body(false),"synthetic-moon",moonMu);
            for(double[] initial:cases) {
                var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
                    new Vector3D(initial[3],initial[4],initial[5])),FramesFactory.getGCRF(),EPOCH,MU);
                var state=new SpacecraftState(orbit);
                var acceleration=Vector3D.ZERO;
                if(mode!=1) acceleration=acceleration.add(sun.acceleration(state,new double[]{sunMu}));
                if(mode!=0) acceleration=acceleration.add(moon.acceleration(state,new double[]{moonMu}));
                for(double dt:new double[]{-86400,-6000,6000,86400}) {
                    var integrator=new DormandPrince853Integrator(.0001,30,
                        new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                        new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
                    var propagator=new NumericalPropagator(integrator);
                    propagator.setOrbitType(OrbitType.CARTESIAN);
                    propagator.setInitialState(state);
                    if(mode!=1) propagator.addForceModel(sun);
                    if(mode!=0) propagator.addForceModel(moon);
                    var pv=propagator.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
                    System.out.printf("%d %.0f",mode,dt);
                    for(double value:initial) System.out.printf(" %.17g",value);
                    for(double value:pv.getPosition().toArray()) System.out.printf(" %.17g",value);
                    for(double value:pv.getVelocity().toArray()) System.out.printf(" %.17g",value);
                    for(double value:acceleration.toArray()) System.out.printf(" %.17g",value);
                    System.out.println();
                }
            }
        }
    }
}
