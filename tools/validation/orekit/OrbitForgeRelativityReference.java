// Independent oracle: unmodified Orekit 13.1.2 force models and DP853 propagation.
// Published IERS 2003/2010 leading relativistic terms are identical here.
// Synthetic geometric ephemeris and a fixed tilted Earth isolate force semantics.
import java.util.Locale;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.*;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.CelestialBody;
import org.orekit.forces.gravity.*;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;

public final class OrbitForgeRelativityReference {
    static final double MU=3.986004415e14, SUN_MU=1.3271244004127942e20;
    static final double R=1.496e11, RATE=2*Math.PI/(365.25*86400), TILT=.409;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static final Frame BODY=new Frame(FRAME,new Transform(EPOCH,
        new Rotation(Vector3D.PLUS_I,.2,RotationConvention.FRAME_TRANSFORM)),"tilted-Earth");
    static Vector3D sunPosition(double t) {
        double a=.3+RATE*t;
        return new Vector3D(R*Math.cos(a),R*Math.sin(a)*Math.cos(TILT),R*Math.sin(a)*Math.sin(TILT));
    }
    static Vector3D sunVelocity(double t) {
        double a=.3+RATE*t;
        return new Vector3D(-R*RATE*Math.sin(a),R*RATE*Math.cos(a)*Math.cos(TILT),R*RATE*Math.cos(a)*Math.sin(TILT));
    }
    static CelestialBody body(boolean earth) {
        return new CelestialBody() {
            public String getName() { return earth ? "Earth" : "Sun"; }
            public double getGM() { return earth ? MU : SUN_MU; }
            public Frame getInertiallyOrientedFrame() { return FRAME; }
            public Frame getBodyOrientedFrame() { return FRAME; }
            // DeSitterRelativity requests Earth relative to the Sun inertial frame.
            public TimeStampedPVCoordinates getPVCoordinates(AbsoluteDate date, Frame frame) {
                if(frame!=FRAME) throw new IllegalArgumentException("Unexpected reference frame");
                double t=date.durationFrom(EPOCH), sign=earth ? -1 : 0;
                return new TimeStampedPVCoordinates(date,sunPosition(t).scalarMultiply(sign),
                    sunVelocity(t).scalarMultiply(sign),sunPosition(t).scalarMultiply(-sign*RATE*RATE));
            }
            public Vector3D getPosition(AbsoluteDate date, Frame frame) { return getPVCoordinates(date,frame).getPosition(); }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date, Frame frame) {
                T a=date.durationFrom(EPOCH).multiply(RATE).add(.3);
                double sign=earth ? -1 : 0;
                return new FieldVector3D<>(a.cos().multiply(sign*R),
                    a.sin().multiply(sign*R*Math.cos(TILT)),a.sin().multiply(sign*R*Math.sin(TILT)));
            }
        };
    }
    static void vector(Vector3D v) { for(double x:v.toArray()) System.out.printf(" %.17g",x); }
    static SpacecraftState state(Vector3D r, Vector3D v, AbsoluteDate date) {
        return new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,v),FRAME,date,MU));
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 Relativity, LenseThirringRelativity, DeSitterRelativity; SI; TAI seconds");
        System.out.println("# A: r[3] v[3] Sun_r[3] Sun_v[3] pole[3] Schwarzschild[3] LenseThirring[3] deSitter[3] total[3]");
        System.out.println("# T: enabled elapsed r[3] v[3]; monopole, tilted pole (0,-sin(.2),cos(.2)), moving synthetic Sun");
        var schwarz=new Relativity(MU);
        var deSitter=new DeSitterRelativity(body(true),body(false));
        for(double t:new double[]{-1e8,0,1e8}) for(double angle:new double[]{0,.2,-.7}) {
            Frame fixed=new Frame(FRAME,new Transform(EPOCH,new Rotation(Vector3D.PLUS_I,angle,RotationConvention.FRAME_TRANSFORM)),"body");
            var lt=new LenseThirringRelativity(MU,fixed);
            var pole=fixed.getStaticTransformTo(FRAME,EPOCH).transformVector(Vector3D.PLUS_K);
            for(double[][] pv:new double[][][]{ {{7e6,0,0},{0,6200,5300}}, {{.001,-.002,7e6},{7300,10,50}},
                    {{4e6,-3e6,5e6},{2000,5000,-4000}}, {{42164e3,0,0},{0,3074,0}}, {{7e6,0,0},{0,0,0}} }) {
                var r=new Vector3D(pv[0]); var v=new Vector3D(pv[1]);
                var s=state(r,v,EPOCH.shiftedBy(t));
                var a=schwarz.acceleration(s,new double[]{MU});
                var b=lt.acceleration(s,new double[]{MU});
                var c=deSitter.acceleration(s,new double[]{SUN_MU});
                System.out.print("A");
                for(var x:new Vector3D[]{r,v,sunPosition(t),sunVelocity(t),pole,a,b,c,a.add(b).add(c)}) vector(x);
                System.out.println();
            }
        }
        for(int enabled=0;enabled<2;enabled++) for(double dt:new double[]{-86400,-6000,6000,86400}) {
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator); prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(state(new Vector3D(7e6,0,0),new Vector3D(0,6200,5300),EPOCH));
            if(enabled==1) { prop.addForceModel(schwarz); prop.addForceModel(new LenseThirringRelativity(MU,BODY)); prop.addForceModel(deSitter); }
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %d %.0f",enabled,dt); vector(pv.getPosition()); vector(pv.getVelocity()); System.out.println();
        }
    }
}
