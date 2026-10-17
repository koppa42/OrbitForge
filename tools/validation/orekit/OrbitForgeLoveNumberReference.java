// Independent reference: unmodified Orekit 13.1.2 SolidTidesField with a custom
// LoveNumbers input. Only real k20 changes; permanent deformation uses the same k20.
// Compile together with OrbitForgeSolidTideReference and OrbitForgeTimeDependentTideReference.
package org.orekit.forces.gravity;
import static org.orekit.forces.gravity.OrbitForgeSolidTideReference.*;
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.potential.*;
import org.orekit.utils.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;

public final class OrbitForgeLoveNumberReference {
    static final IERSConventions IERS=IERSConventions.IERS_2003;
    static final double[] VALUES={0,.3,.30190,.4};
    static double permanent(double k20) { return IERS.getPermanentTide()*k20/.30190; }
    static SolidTidesField custom(double k20, boolean zero, boolean secondary) {
        var original=IERS.getLoveNumbers();
        double[][] real=new double[original.getSize()][],imag=new double[original.getSize()][],plus=new double[original.getSize()][];
        for(int n=0;n<original.getSize();n++) {
            real[n]=new double[n+1];imag[n]=new double[n+1];plus[n]=new double[n+1];
            for(int m=0;m<=n;m++) {
                real[n][m]=original.getReal(n,m);imag[n][m]=original.getImaginary(n,m);plus[n][m]=original.getPlus(n,m);
            }
        }
        real[2][0]=k20;
        return new SolidTidesField(new LoveNumbers(real,imag,plus),
            secondary ? OrbitForgeTimeDependentTideReference.frequency(-31.7) : ZERO,
            permanent(k20),secondary ? OrbitForgeTimeDependentTideReference.pole(EPOCH,
                .12*Constants.ARC_SECONDS_TO_RADIANS,.25*Constants.ARC_SECONDS_TO_RADIANS) : null,
            FRAME,R,MU,zero ? TideSystem.ZERO_TIDE : TideSystem.TIDE_FREE,body(true,0),body(false,0));
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 SolidTidesField / LoveNumbers / Holmes-Featherstone; Hipparchus 4.0.2 DP853");
        System.out.println("# Custom real k20; other Love numbers IERS_2003; UTC=TAI-32 s; identity body frame; moving Sun/Moon");
        System.out.println("# secondary=1: 71 frequency terms + printed 2003 pole; UT1-TAI=-31.7 s, xp=.12 arcsec, yp=.25 arcsec");
        System.out.println("# C: k20 zero_tide secondary elapsed; normalized C,S for (2,0..2),(3,0..3),(4,0..2)");
        System.out.println("# A: k20 zero_tide secondary degree order truncate x y z ax ay az; epoch 0; C20 base + matching permanent if zero_tide");
        System.out.println("# T: k20 secondary elapsed rx ry rz vx vy vz; 0/0 base, untruncated Full tides; no direct third-body force");
        for(double k20:VALUES) for(int zero=0;zero<2;zero++) for(int secondary=0;secondary<2;secondary++) {
            var tide=custom(k20,zero==1,secondary==1);
            for(double elapsed:new double[]{-6000,0,6000}) {
                var h=tide.onDate(EPOCH.shiftedBy(elapsed));
                System.out.printf("C %.17g %d %d %.0f",k20,zero,secondary,elapsed);
                for(int n=2;n<=4;n++) for(int m=0;m<=(n==4?2:n);m++)
                    System.out.printf(" %.17g %.17g",h.getNormalizedCnm(n,m),h.getNormalizedSnm(n,m));
                System.out.println();
            }
            for(int[] cut:new int[][]{{0,0,0},{2,0,1},{4,3,1}}) {
                int d=cut[0],o=cut[1],truncate=cut[2],td=truncate==1?d:4,to=truncate==1?o:3;
                var h=custom(k20,zero==1 && d>=2,secondary==1).onDate(EPOCH);
                int max=Math.max(d,td);double[][] c=new double[max+1][],s=new double[max+1][];
                for(int n=0;n<=max;n++) {
                    c[n]=new double[n+1];s[n]=new double[n+1];
                    if(n==0)c[n][0]=1;
                    if(n==2 && d>=2)c[n][0]=C20+(zero==1?permanent(k20):0);
                    if(n<=td)for(int m=0;m<=Math.min(n,to);m++) {
                        c[n][m]+=h.getNormalizedCnm(n,m);s[n][m]+=h.getNormalizedSnm(n,m);
                    }
                }
                var force=new HolmesFeatherstoneAttractionModel(FRAME,
                    GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.ZERO_TIDE,c,s));
                for(double[] position:new double[][]{{4e6,-3e6,5e6},{.001,-.002,7e6}}) {
                    var r=new Vector3D(position);
                    var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(0,6200,5300)),FRAME,EPOCH,MU));
                    var a=force.acceleration(state,new double[]{MU}).add(r.scalarMultiply(-MU/Math.pow(r.getNorm(),3)));
                    System.out.printf("A %.17g %d %d %d %d %d",k20,zero,secondary,d,o,truncate);
                    for(double v:position)System.out.printf(" %.17g",v);
                    for(double v:a.toArray())System.out.printf(" %.17g",v);
                    System.out.println();
                }
            }
        }
        for(double k20:VALUES)for(int secondary=0;secondary<2;secondary++)for(double dt:new double[]{-86400,86400}) {
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(new Vector3D(7e6,0,0),
                new Vector3D(0,6200,5300)),FRAME,EPOCH,MU)));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,custom(k20,false,secondary==1)));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %.17g %d %.0f",k20,secondary,dt);
            for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
            for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
}
