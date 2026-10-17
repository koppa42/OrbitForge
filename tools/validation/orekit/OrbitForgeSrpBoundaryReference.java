// Independent spherical shadow events and trajectories: Orekit 13.1.2 / Hipparchus 4.0.2.
// Cylinder uses Orekit EclipseDetector with a point luminous source (radius zero),
// retaining the spacecraft-to-Sun direction specified by STK. Orekit's named
// CylindricalShadowEclipseDetector instead uses the geocentric Sun direction.
import java.util.*;
import java.util.stream.Stream;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.FieldVector3D;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.forces.ForceModel;
import org.orekit.forces.radiation.*;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.events.*;
import org.orekit.propagation.events.handlers.*;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.utils.*;

public final class OrbitForgeSrpBoundaryReference {
    static final double MU=OrbitForgeSrpReference.MU, AU=OrbitForgeSrpReference.AU;
    static final double CR=1.7, AREA_MASS=20, PRESSURE=4.56e-6;
    static record Row(int kind, double time, double[] state) {}
    static List<Row> run(int model, int orbitCase, int direction, double step) {
        final var frame=FramesFactory.getGCRF();
        final var epoch=OrbitForgeSrpReference.EPOCH;
        final var sun=OrbitForgeSrpReference.sun();
        final var shape=new OneAxisEllipsoid(6378137,0,frame);
        final double r=orbitCase==0 ? 7e6 : 42164000;
        final var pv=new PVCoordinates(new Vector3D(r,0,0),
            orbitCase==0 ? new Vector3D(0,6200,5300) : new Vector3D(0,Math.sqrt(MU/r),0));
        final var state=new SpacecraftState(new CartesianOrbit(pv,frame,epoch,MU)).withMass(1000);
        final var settings=new EventDetectionSettings(60,1e-10,200);
        final var outer=new EclipseDetector(sun,model==0 ? 0 : Constants.SUN_RADIUS,shape)
            .withPenumbra().withDetectionSettings(settings).withHandler(new ResetDerivativesOnEvent());
        final var inner=new EclipseDetector(sun,Constants.SUN_RADIUS,shape)
            .withUmbra().withDetectionSettings(settings).withHandler(new ResetDerivativesOnEvent());
        final var integrator=new DormandPrince853Integrator(1e-8,step,
            new double[]{1e-8,1e-8,1e-8,1e-11,1e-11,1e-11,1e-6},
            new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
        final var propagator=new NumericalPropagator(integrator);
        propagator.setOrbitType(OrbitType.CARTESIAN);
        propagator.setInitialState(state);
        if (model==1) {
            propagator.addForceModel(new SolarRadiationPressure(AU,PRESSURE,sun,shape,
                new IsotropicRadiationSingleCoefficient(AREA_MASS*1000,CR),settings));
        } else {
            propagator.addForceModel(new ForceModel() {
                public boolean dependsOnPositionOnly() { return true; }
                public List<ParameterDriver> getParametersDrivers() { return List.of(); }
                public Vector3D acceleration(SpacecraftState s, double[] p) {
                    if (outer.g(s)<0) return Vector3D.ZERO;
                    var away=s.getPosition().subtract(sun.getPosition(s.getDate(),s.getFrame()));
                    return away.scalarMultiply(PRESSURE*AU*AU*CR*AREA_MASS/Math.pow(away.getNorm(),3));
                }
                public <T extends CalculusFieldElement<T>> FieldVector3D<T> acceleration(
                    FieldSpacecraftState<T> s,T[] p) { throw new UnsupportedOperationException(); }
            });
        }
        final var logger=new EventsLogger();
        propagator.addEventDetector(logger.monitorDetector(outer));
        if (model==1) propagator.addEventDetector(logger.monitorDetector(inner));
        final double end=direction*(orbitCase==0 ? 6000 : 90000);
        final var result=propagator.propagate(epoch.shiftedBy(end));
        final List<Row> rows=new ArrayList<>();
        for(var event:logger.getLoggedEvents()) {
            var s=event.getState();
            rows.add(new Row(event.getEventDetector()==outer ? 0 : 1,
                            s.getDate().durationFrom(epoch),values(s)));
        }
        rows.add(new Row(9,end,values(result)));
        return rows;
    }
    static double[] values(SpacecraftState s) {
        var p=s.getPosition().toArray(); var v=s.getPVCoordinates().getVelocity().toArray();
        return new double[]{p[0],p[1],p[2],v[0],v[1],v[2]};
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2 DP853; TAI/GCRF; moving geometric Sun; spherical Earth");
        System.out.println("# model(0 cylinder,1 dual) orbit(0 LEO,1 GEO) direction kind(0 outer,1 umbra,9 final) seconds state[6]");
        System.out.println("# MU=3.986004415e14 Earth=6378137 Sun=695700000 AU=149597870700 P=4.56e-6 Cr=1.7 A/m=20");
        double maxP=0,maxV=0,maxT=0;
        for(int model=0;model<2;model++) for(int orbit=0;orbit<2;orbit++) for(int direction:new int[]{-1,1}) {
            var coarse=run(model,orbit,direction,2); var fine=run(model,orbit,direction,1);
            if(coarse.size()!=fine.size()) throw new AssertionError("Event count failed convergence");
            for(int i=0;i<fine.size();i++) {
                var a=coarse.get(i); var b=fine.get(i);
                if(a.kind()!=b.kind()) throw new AssertionError("Event order changed");
                maxT=Math.max(maxT,Math.abs(a.time()-b.time()));
                for(int k=0;k<6;k++) {
                    double difference=Math.abs(a.state()[k]-b.state()[k]);
                    if(k<3) maxP=Math.max(maxP,difference); else maxV=Math.max(maxV,difference);
                }
                System.out.printf("%d %d %d %d %.17g",model,orbit,direction,b.kind(),b.time());
                for(double x:b.state()) System.out.printf(" %.17g",x);
                System.out.println();
            }
        }
        System.out.printf("# step-halving maxima position=%.17g m velocity=%.17g m/s event=%.17g s%n",maxP,maxV,maxT);
        if(maxP>5e-4 || maxV>2e-7 || maxT>1e-7) throw new AssertionError("Reference convergence failed");
    }
}
