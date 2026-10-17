// Independent Orekit 13.1.2 NRLMSISE-00/DragForce + Hipparchus 4.0.2 DP853.
// TAI is deliberately used as a synthetic UTC clock; no real EOP/ephemeris differences.
import java.util.Locale;
import java.util.Arrays;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.*;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.frames.*;
import org.orekit.forces.drag.*;
import org.orekit.models.earth.atmosphere.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.*;
import org.orekit.utils.*;

public class OrbitForgeDragReference {
    static final double MU=3.986004415e14,AU=149597870700.0,OMEGA=7.292115e-5;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static ExtendedPositionProvider sun() {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame) {
                double phase=.03+date.durationFrom(EPOCH)*2*Math.PI/(365.25*86400);
                return new Vector3D(AU*Math.cos(phase),AU*Math.sin(phase)*Math.cos(.409),AU*Math.sin(phase)*Math.sin(.409));
            }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                T phase=date.durationFrom(EPOCH).multiply(2*Math.PI/(365.25*86400)).add(.03);
                return new FieldVector3D<>(phase.cos().multiply(AU),phase.sin().multiply(AU*Math.cos(.409)),phase.sin().multiply(AU*Math.sin(.409)));
            }
        };
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        Frame frame=FramesFactory.getGCRF();
        Frame earthFrame=new Frame(frame,new TransformProvider() {
            public Transform getTransform(AbsoluteDate date) {
                Rotation rotation=new Rotation(Vector3D.PLUS_K,-OMEGA*date.durationFrom(EPOCH),RotationConvention.VECTOR_OPERATOR);
                return new Transform(date,rotation,new Vector3D(0,0,OMEGA));
            }
            public <T extends CalculusFieldElement<T>> FieldTransform<T> getTransform(FieldAbsoluteDate<T> date) {
                return new FieldTransform<>(date.getField(),getTransform(date.toAbsoluteDate()));
            }
        },"synthetic-rotating-earth");
        var earth=new OneAxisEllipsoid(6378137,1/298.257223563,earthFrame);
        double[][] initialStates={{6778137,0,0,0,6300,4300},{7078137,0,0,0,5200,5400}};
        double[][] weather={{150,150,3,2.2,.02},{70,80,0,2.2,.02},{250,220,8.5,2.7,.035}};
        System.out.println("# Orekit 13.1.2 NRLMSISE00 gtd7d, co-rotating WGS84; synthetic UTC/TAI; moving geometric Sun; DP853");
        System.out.println("# mode dt initial_r[3] initial_v[3] expected_r[3] expected_v[3] drag_acceleration_initial[3]");
        for(int mode=0;mode<weather.length;mode++) {
            final double[] w=weather[mode];
            double[] kpTable={0,2,3,4,5,6,7,9,12,15,18,22,27,32,39,48,56,67,80,94,111,132,154,179,207,236,300,400};
            int index=(int)(w[2]*3); final double ap=kpTable[index]+(w[2]*3-index)*(kpTable[index+1]-kpTable[index]);
            var parameters=new NRLMSISE00InputParameters() {
                public AbsoluteDate getMinDate(){return AbsoluteDate.PAST_INFINITY;}
                public AbsoluteDate getMaxDate(){return AbsoluteDate.FUTURE_INFINITY;}
                public double getDailyFlux(AbsoluteDate date){return w[0];}
                public double getAverageFlux(AbsoluteDate date){return w[1];}
                public double[] getAp(AbsoluteDate date){double[] a=new double[7];Arrays.fill(a,ap);return a;}
            };
            var atmosphere=new NRLMSISE00(parameters,sun(),earth,TimeScalesFactory.getTAI());
            var drag=new DragForce(atmosphere,new IsotropicDrag(w[4]*1000,w[3]));
            for(double[] initial:initialStates) {
                var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
                    new Vector3D(initial[3],initial[4],initial[5])),frame,EPOCH,MU);
                var state=new SpacecraftState(orbit).withMass(1000);
                var acceleration=drag.acceleration(state,drag.getParameters(EPOCH));
                for(double dt:new double[]{-6000,-3600,3600,6000}) {
                    var integrator=new DormandPrince853Integrator(1e-7,1,
                        new double[]{1e-8,1e-8,1e-8,1e-11,1e-11,1e-11,1e-6},
                        new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
                    var propagator=new NumericalPropagator(integrator);
                    propagator.setOrbitType(OrbitType.CARTESIAN);propagator.setInitialState(state);propagator.addForceModel(drag);
                    var pv=propagator.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
                    System.out.printf("%d %.0f",mode,dt);
                    for(double value:initial)System.out.printf(" %.17g",value);
                    for(double value:pv.getPosition().toArray())System.out.printf(" %.17g",value);
                    for(double value:pv.getVelocity().toArray())System.out.printf(" %.17g",value);
                    for(double value:acceleration.toArray())System.out.printf(" %.17g",value);
                    System.out.println();
                }
            }
        }
    }
}
