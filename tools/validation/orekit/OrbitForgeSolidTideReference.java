// Independent reference: calls unmodified Orekit 13.1.2 SolidTidesField and
// Holmes-Featherstone with IERS_2003 Love numbers, frequency/pole terms disabled.
// Package access exposes Orekit's coefficient provider without copying its algorithm.
package org.orekit.forces.gravity;
import java.util.Locale;
import java.util.Arrays;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.geometry.euclidean.threed.FieldVector3D;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.util.MathArrays;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.bodies.CelestialBody;
import org.orekit.forces.gravity.potential.GravityFieldFactory;
import org.orekit.forces.gravity.potential.TideSystem;
import org.orekit.frames.Frame;
import org.orekit.frames.FramesFactory;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;

public final class OrbitForgeSolidTideReference {
    static final double MU=3.986004415e14, R=6378136.3, C20=-4.84165371736e-4;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static final TimeVectorFunction ZERO = new TimeVectorFunction() {
        public double[] value(AbsoluteDate date) { return new double[5]; }
        public <T extends CalculusFieldElement<T>> T[] value(FieldAbsoluteDate<T> date) {
            T[] a=MathArrays.buildArray(date.getField(),5); Arrays.fill(a,date.getField().getZero()); return a;
        }
    };
    static CelestialBody body(boolean sun, int geometry) {
        final double radius=sun ? 1.496e11 : 384400000;
        final double rate=2*Math.PI/(sun ? 365.25*86400 : 27.321661*86400);
        final double phase=(sun ? .3 : 1.2)+geometry, tilt=sun ? .409 : .5;
        return new CelestialBody() {
            public String getName() { return sun ? "Sun" : "Moon"; }
            public double getGM() { return sun ? 1.3271244004127942e20 : 4.9028001184575496e12; }
            public Frame getInertiallyOrientedFrame() { return FRAME; }
            public Frame getBodyOrientedFrame() { return FRAME; }
            public Vector3D getPosition(AbsoluteDate date, Frame frame) {
                double angle=phase+rate*date.durationFrom(EPOCH);
                return new Vector3D(radius*Math.cos(angle),radius*Math.sin(angle)*Math.cos(tilt),
                                    radius*Math.sin(angle)*Math.sin(tilt));
            }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date, Frame frame) {
                T angle=date.durationFrom(EPOCH).multiply(rate).add(phase);
                return new FieldVector3D<>(angle.cos().multiply(radius),
                    angle.sin().multiply(radius*Math.cos(tilt)),angle.sin().multiply(radius*Math.sin(tilt)));
            }
        };
    }
    static SolidTidesField tides(boolean zero, int geometry) {
        var iers=IERSConventions.IERS_2003;
        return new SolidTidesField(iers.getLoveNumbers(),ZERO,iers.getPermanentTide(),null,
            FRAME,R,MU,zero ? TideSystem.ZERO_TIDE : TideSystem.TIDE_FREE,body(true,geometry),body(false,geometry));
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2; IERS 2003 Love numbers; frequency/pole off; k20=.30190");
        System.out.println("# A: zero_tide geometry degree order truncate x y z ax ay az");
        System.out.println("# T: elapsed rx ry rz vx vy vz; moving bodies geometry 0, 0/0 gravity, untruncated full tides");
        for(int zero=0;zero<2;zero++) for(int geometry=0;geometry<3;geometry++) {
            for(int[] cut:new int[][]{{0,0},{2,0},{2,2},{4,3},{21,21}}) for(int truncate=0;truncate<2;truncate++) {
                int d=cut[0],o=cut[1],td=truncate==1 ? Math.min(4,d) : 4,to=truncate==1 ? Math.min(4,o) : 4;
                var harmonics=tides(zero==1 && d>=2,geometry).onDate(EPOCH);
                int max=Math.max(d,td);
                double[][] c=new double[max+1][],s=new double[max+1][];
                for(int n=0;n<=max;n++) {
                    c[n]=new double[n+1]; s[n]=new double[n+1];
                    if(n==0) c[n][0]=1;
                    if(n==2 && d>=2) c[n][0]=C20+(zero==1 ? IERSConventions.IERS_2003.getPermanentTide() : 0);
                    if(n<=td) for(int m=0;m<=Math.min(n,to);m++) {
                        c[n][m]+=harmonics.getNormalizedCnm(n,m); s[n][m]+=harmonics.getNormalizedSnm(n,m);
                    }
                }
                var provider=GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.ZERO_TIDE,c,s);
                var force=new HolmesFeatherstoneAttractionModel(FRAME,provider);
                for(double[] position:new double[][]{{4e6,-3e6,5e6},{.001,-.002,7e6}}) {
                    var r=new Vector3D(position);
                    var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(0,6200,5300)),FRAME,EPOCH,MU));
                    var a=force.acceleration(state,new double[]{MU}).add(r.scalarMultiply(-MU/Math.pow(r.getNorm(),3)));
                    System.out.printf("A %d %d %d %d %d",zero,geometry,d,o,truncate);
                    for(double v:position) System.out.printf(" %.17g",v);
                    for(double v:a.toArray()) System.out.printf(" %.17g",v);
                    System.out.println();
                }
            }
        }
        for(double dt:new double[]{-86400,-6000,6000,86400}) {
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);
            prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(new Vector3D(7e6,0,0),
                new Vector3D(0,6200,5300)),FRAME,EPOCH,MU)));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,tides(false,0)));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %.0f",dt);
            for(double v:pv.getPosition().toArray()) System.out.printf(" %.17g",v);
            for(double v:pv.getVelocity().toArray()) System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
}
