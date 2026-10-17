// Independent Orekit 13.1.2 / Hipparchus 4.0.2 area-and-mass verification.
// The Sun and synthetic rotating Earth match OrbitForgeDragReference, with UTC=TAI.
import java.util.Locale;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.*;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.frames.*;
import org.orekit.forces.drag.*;
import org.orekit.forces.radiation.*;
import org.orekit.models.earth.atmosphere.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.events.EventDetectionSettings;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.*;
import org.orekit.utils.*;

public final class OrbitForgeMassReference {
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        final var epoch=OrbitForgeDragReference.EPOCH;
        final var sun=OrbitForgeDragReference.sun();
        final var frame=FramesFactory.getGCRF();
        final var earthFrame=new Frame(frame,new TransformProvider() {
            public Transform getTransform(AbsoluteDate date) {
                final double spin=OrbitForgeDragReference.OMEGA;
                var rotation=new Rotation(Vector3D.PLUS_K,-spin*date.durationFrom(epoch),
                    RotationConvention.VECTOR_OPERATOR);
                return new Transform(date,rotation,new Vector3D(0,0,spin));
            }
            public <T extends CalculusFieldElement<T>> FieldTransform<T> getTransform(FieldAbsoluteDate<T> date) {
                return new FieldTransform<>(date.getField(),getTransform(date.toAbsoluteDate()));
            }
        },"synthetic-rotating-earth");
        final var earth=new OneAxisEllipsoid(6378137,1/298.257223563,earthFrame);
        final var weather=new NRLMSISE00InputParameters() {
            public AbsoluteDate getMinDate(){return AbsoluteDate.PAST_INFINITY;}
            public AbsoluteDate getMaxDate(){return AbsoluteDate.FUTURE_INFINITY;}
            public double getDailyFlux(AbsoluteDate date){return 150;}
            public double getAverageFlux(AbsoluteDate date){return 150;}
            public double[] getAp(AbsoluteDate date){return new double[]{15,15,15,15,15,15,15};}
        };
        final var atmosphere=new NRLMSISE00(weather,sun,earth,TimeScalesFactory.getTAI());
        // Explicit mass/areas: equal A/m across four masses; then changed A/m and zero areas.
        final double[][] models={{1,.02,.03},{250,5,7.5},{1000,20,30},{2700,54,81},
                                  {250,10,15},{1000,0,30},{1000,20,0}};
        final double[][] states={{6778137,0,0,0,6300,4300},{7078137,0,0,0,5200,5400}};
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2 DP853; NRLMSISE00, Cd=2.2, F107=F107avg=150, Ap=15");
        System.out.println("# Spherical SRP Cr=1.7, P=4.56e-6 Pa at AU, geometric Sun, spherical Earth dual cone; UTC=TAI");
        System.out.println("# mass_kg drag_area_m2 srp_area_m2 dt initial_r[3] initial_v[3] expected_r[3] expected_v[3] drag_a[3] srp_a[3]");
        for(double[] model:models) {
            var drag=new DragForce(atmosphere,new IsotropicDrag(model[1],2.2));
            var srp=new SolarRadiationPressure(OrbitForgeDragReference.AU,4.56e-6,sun,
                new OneAxisEllipsoid(6378137,0,frame),
                new IsotropicRadiationSingleCoefficient(model[2],1.7),
                new EventDetectionSettings(60,1e-8,100));
            for(double[] initial:states) {
                var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
                    new Vector3D(initial[3],initial[4],initial[5])),frame,epoch,OrbitForgeDragReference.MU);
                var state=new SpacecraftState(orbit).withMass(model[0]);
                var da=drag.acceleration(state,drag.getParameters(epoch));
                var sa=srp.acceleration(state,srp.getParameters(epoch));
                for(double dt:new double[]{-6000,6000}) {
                    var integrator=new DormandPrince853Integrator(1e-7,1,
                        new double[]{1e-8,1e-8,1e-8,1e-11,1e-11,1e-11,1e-6},
                        new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
                    var propagator=new NumericalPropagator(integrator);
                    propagator.setOrbitType(OrbitType.CARTESIAN);
                    propagator.setInitialState(state);
                    propagator.addForceModel(drag);
                    propagator.addForceModel(srp);
                    var pv=propagator.propagate(epoch.shiftedBy(dt)).getPVCoordinates();
                    for(double v:model)System.out.printf("%.17g ",v);
                    System.out.printf("%.0f",dt);
                    for(double v:initial)System.out.printf(" %.17g",v);
                    for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
                    for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
                    for(double v:da.toArray())System.out.printf(" %.17g",v);
                    for(double v:sa.toArray())System.out.printf(" %.17g",v);
                    System.out.println();
                }
            }
        }
    }
}
