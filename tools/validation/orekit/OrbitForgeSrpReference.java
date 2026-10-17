// Independent trajectories: Orekit 13.1.2 / Hipparchus 4.0.2 DP853 and eclipse events.
// Synthetic geometric Sun positions, spherical Earth, and explicitly recorded constants.
import java.util.Locale;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.FieldVector3D;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.forces.radiation.IsotropicRadiationSingleCoefficient;
import org.orekit.forces.radiation.SolarRadiationPressure;
import org.orekit.frames.Frame;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.orbits.OrbitType;
import org.orekit.propagation.SpacecraftState;
import org.orekit.propagation.events.EventDetectionSettings;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.FieldAbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.Constants;
import org.orekit.utils.ExtendedPositionProvider;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeSrpReference {
    static final double MU=3.986004415e14, AU=149597870700.0, EARTH=6378137.0;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static ExtendedPositionProvider sun() {
        final double rate=2*Math.PI/(365.25*86400), tilt=.409;
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date, Frame frame) {
                double angle=.03+rate*date.durationFrom(EPOCH);
                return new Vector3D(AU*Math.cos(angle), AU*Math.sin(angle)*Math.cos(tilt),
                                    AU*Math.sin(angle)*Math.sin(tilt));
            }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date, Frame frame) {
                T angle=date.durationFrom(EPOCH).multiply(rate).add(.03);
                return new FieldVector3D<>(angle.cos().multiply(AU),
                    angle.sin().multiply(AU*Math.cos(tilt)), angle.sin().multiply(AU*Math.sin(tilt)));
            }
        };
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2 DP853; spherical Earth; moving geometric Sun; TAI/GCRF");
        System.out.println("# mode dt initial_r[3] initial_v[3] expected_r[3] expected_v[3] srp_acceleration_initial[3] sun_radius_m");
        final var frame=FramesFactory.getGCRF();
        final var position=sun();
        double[][] cases={{7e6,0,0,0,6200,5300},{42164000,0,0,0,Math.sqrt(MU/42164000),0}};
        for(int mode=0;mode<3;mode++) {
            double altitude=mode==1 ? 23000 : 0, cr=mode==2 ? 1.7 : 1, area=mode==2 ? 25 : 20;
            var shape=new OneAxisEllipsoid(EARTH+altitude,0,frame);
            var srp=new SolarRadiationPressure(AU,4.56e-6,position,shape,
                new IsotropicRadiationSingleCoefficient(area,cr),new EventDetectionSettings(60,1e-8,100));
            for(double[] initial:cases) {
                var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
                    new Vector3D(initial[3],initial[4],initial[5])),frame,EPOCH,MU);
                var state=new SpacecraftState(orbit).withMass(1000);
                var acceleration=srp.acceleration(state,srp.getParameters(EPOCH));
                for(double dt:new double[]{-86400,-6000,6000,86400}) {
                    var integrator=new DormandPrince853Integrator(1e-6,1,
                        new double[]{1e-8,1e-8,1e-8,1e-11,1e-11,1e-11,1e-6},
                        new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
                    var propagator=new NumericalPropagator(integrator);
                    propagator.setOrbitType(OrbitType.CARTESIAN);
                    propagator.setInitialState(state);
                    propagator.addForceModel(srp);
                    var pv=propagator.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
                    System.out.printf("%d %.0f",mode,dt);
                    for(double value:initial) System.out.printf(" %.17g",value);
                    for(double value:pv.getPosition().toArray()) System.out.printf(" %.17g",value);
                    for(double value:pv.getVelocity().toArray()) System.out.printf(" %.17g",value);
                    for(double value:acceleration.toArray()) System.out.printf(" %.17g",value);
                    System.out.printf(" %.17g%n",Constants.SUN_RADIUS);
                }
            }
        }
    }
}
