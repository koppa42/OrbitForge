// Independent unmodified Orekit 13.1.2 IERS_2003 frequency and pole tides.
// Synthetic UTC = TAI - 32 s (1972 onward), constant EOP, identity body frame.
// Same explicit inputs are used by C++ tests; no OrbitForge computation is invoked.
package org.orekit.forces.gravity;
import java.util.*;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.frames.*;
import org.orekit.forces.gravity.potential.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;

public final class OrbitForgeTimeDependentTideReference {
    static final IERSConventions IERS=IERSConventions.IERS_2003;
    static final TimeScales TIMES=TimeScales.of(
        List.of(new OffsetModel(new DateComponents(1972,1,1),32)), (c,t)->List.of());
    static final AbsoluteDate EPOCH=OrbitForgeSolidTideReference.EPOCH;
    static final Frame FRAME=OrbitForgeSolidTideReference.FRAME;
    static final double R=OrbitForgeSolidTideReference.R, MU=OrbitForgeSolidTideReference.MU;
    static TimeVectorFunction frequency(double ut1Tai) {
        return IERS.getTideFrequencyDependenceFunction(
            new ConstantOffsetTimeScale("synthetic UT1",new TimeOffset(ut1Tai)),TIMES);
    }
    static TimeVectorFunction pole(AbsoluteDate center,double xp,double yp) {
        var entries=new ArrayList<EOPEntry>();
        for(int i=-3;i<=3;i++) {
            var date=center.shiftedBy(i*86400.0);
            int mjd=date.getComponents(TIMES.getUTC()).getDate().getMJD();
            entries.add(new EOPEntry(mjd,0,0,xp,yp,0,0,0,0,0,0,ITRFVersion.ITRF_2014,date));
        }
        return IERS.getSolidPoleTide(new EOPHistory(IERS,3,entries,true,TIMES));
    }
    static SolidTidesField tides(double ut1Tai,double xp,double yp) {
        return new SolidTidesField(IERS.getLoveNumbers(),frequency(ut1Tai),IERS.getPermanentTide(),
            pole(EPOCH,xp,yp),FRAME,R,MU,TideSystem.TIDE_FREE,
            OrbitForgeSolidTideReference.body(true,0),OrbitForgeSolidTideReference.body(false,0));
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 IERS_2003; 71 frequency terms; printed 2003 pole signs; annual.pole");
        System.out.println("# Synthetic UTC=TAI-32 s; no EOP tidal correction; identity body frame; k20=.30190");
        System.out.println("# C: TAI_ns UT1-TAI xp_rad yp_rad frequency(C20,C21,S21,C22,S22) pole(C21,S21)");
        System.out.println("# A: degree order truncate x y z ax ay az; epoch 0, UT1-TAI=-31.7, xp=.12 arcsec, yp=.25 arcsec");
        System.out.println("# T: elapsed rx ry rz vx vy vz; 0/0 gravity, untruncated Full with time terms; same EOP as A");
        long[] epochs={-880000000123456789L,-432000000123456789L,-31622400123456789L,
            -86400000000000L,-43169000000000L,-43168000000000L,-43167000000000L,
            -1L,0L,123456789L,86400123456789L,536500868000000000L,840000000123456789L};
        for(long ns:epochs) for(double[] e:new double[][]{{-31.7,.12,.25},{-33.2,-.2,.5},{0,0,0}}) {
            var date=EPOCH.shiftedBy(new TimeOffset(ns/1000000000L,(ns%1000000000L)*1000000000L));
            double xp=e[1]*Constants.ARC_SECONDS_TO_RADIANS,yp=e[2]*Constants.ARC_SECONDS_TO_RADIANS;
            var f=frequency(e[0]).value(date);var p=pole(date,xp,yp).value(date);
            System.out.printf("C %d %.17g %.17g %.17g",ns,e[0],xp,yp);
            for(double v:f) System.out.printf(" %.17g",v);
            for(double v:p) System.out.printf(" %.17g",v);
            System.out.println();
        }
        double xp=.12*Constants.ARC_SECONDS_TO_RADIANS,yp=.25*Constants.ARC_SECONDS_TO_RADIANS;
        var tide=tides(-31.7,xp,yp);
        for(int[] cut:new int[][]{{0,0},{2,0},{2,1},{2,2},{4,3},{21,21}}) for(int truncate=0;truncate<2;truncate++) {
            int d=cut[0],o=cut[1],td=truncate==1?Math.min(4,d):4,to=truncate==1?Math.min(3,o):3;
            int max=Math.max(d,td);var harmonics=tide.onDate(EPOCH);
            double[][] c=new double[max+1][],s=new double[max+1][];
            for(int n=0;n<=max;n++) {
                c[n]=new double[n+1];s[n]=new double[n+1];
                if(n==0)c[n][0]=1;
                if(n==2 && d>=2)c[n][0]=OrbitForgeSolidTideReference.C20;
                if(n<=td)for(int m=0;m<=Math.min(n,to);m++) {
                    c[n][m]+=harmonics.getNormalizedCnm(n,m);s[n][m]+=harmonics.getNormalizedSnm(n,m);
                }
            }
            var model=new HolmesFeatherstoneAttractionModel(FRAME,
                GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.ZERO_TIDE,c,s));
            for(double[] position:new double[][]{{4e6,-3e6,5e6},{.001,-.002,7e6}}) {
                var r=new Vector3D(position);
                var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(0,6200,5300)),FRAME,EPOCH,MU));
                var a=model.acceleration(state,new double[]{MU}).add(r.scalarMultiply(-MU/Math.pow(r.getNorm(),3)));
                System.out.printf("A %d %d %d",d,o,truncate);
                for(double v:position)System.out.printf(" %.17g",v);
                for(double v:a.toArray())System.out.printf(" %.17g",v);
                System.out.println();
            }
        }
        for(double dt:new double[]{-86400,-6000,6000,86400}) {
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(new Vector3D(7e6,0,0),
                new Vector3D(0,6200,5300)),FRAME,EPOCH,MU)));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,tide));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %.0f",dt);
            for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
            for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
}
