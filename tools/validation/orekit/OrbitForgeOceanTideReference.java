// Independent OceanTidesField / HolmesFeatherstone / DP853 reference, Orekit 13.1.2.
// Compile with the fixed JARs in dependencies.json. Args: potential-file maximum-degree.
// No OrbitForge implementation; original IERS 2010 mean-pole model, no EOP interpolation effects.
package org.orekit.forces.gravity;
import java.util.*;
import java.nio.file.*;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.frames.*;
import org.orekit.forces.gravity.potential.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;
public final class OrbitForgeOceanTideReference {
    static final IERSConventions IERS=IERSConventions.IERS_2010;
    static final TimeScales TIMES=TimeScales.of(List.of(new OffsetModel(new DateComponents(1972,1,1),32)),(c,t)->List.of());
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TIMES.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static final double MU=3.986004415e14,R=6378136.3;
    static OceanTidesField field(Path path,int degree,int order,AbsoluteDate center,double ut1Tai,double xp,double yp,boolean pole) throws Exception {
        var reader=new FESCnmSnmReader(".*",1e-11);
        reader.setMaxParseDegree(degree);reader.setMaxParseOrder(order);
        try(var input=Files.newInputStream(path)){reader.loadData(input,path.toString());}
        var entries=new ArrayList<EOPEntry>();
        for(int i=-3;i<=3;i++) {
            var date=center.shiftedBy(i*86400.0);int mjd=date.getComponents(TIMES.getUTC()).getDate().getMJD();
            entries.add(new EOPEntry(mjd,ut1Tai+32,0,xp,yp,0,0,0,0,0,0,ITRFVersion.ITRF_2014,date));
        }
        return new OceanTidesField(R,MU,reader.getWaves(),
            IERS.getNutationArguments(new ConstantOffsetTimeScale("synthetic UT1",new TimeOffset(ut1Tai)),TIMES),
            pole?IERS.getOceanPoleTide(new EOPHistory(IERS,3,entries,true,TIMES)):null);
    }
    static String verifyInput(Path path, int maximum) throws Exception {
        if(maximum!=4 && maximum!=100)throw new IllegalArgumentException("Expected 4 or 100 degree reference input");
        var hash=HexFormat.of().formatHex(java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path)));
        var expected=maximum==4?"ed53a1225d6e720264bbe22207bbd60933518c4552cd09a0f98d8e0a98cf70d9":
            "74156304aca8f66ed1f67775de3d4a9d24ecb94abb47127ef40d5bb4630f719f";
        if(!hash.equals(expected))throw new IllegalArgumentException("Unexpected FES potential SHA-256: "+hash);
        return hash;
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);var path=Path.of(args[0]);
        int maximum=args.length>1?Integer.parseInt(args[1]):100;
        var hash=verifyInput(path,maximum);
        System.out.println("# Independent unmodified Orekit 13.1.2 FESCnmSnmReader / OceanTidesField / IERS_2010 / HolmesFeatherstone");
        System.out.println("# FES2014b.v1 with S1, sha256="+hash+"; scale 1e-11; 36 explicit waves; no inferred minor waves");
        System.out.println("# S_n0 is canonicalized to zero: sin(0*longitude) is identically zero.");
        System.out.println("# IERS_2010 original cubic/linear mean pole, 2010-01-01 00:00 TT switch; identity body frame, UTC=TAI-32s");
        System.out.println("# C ns ut1Tai xp yp degree order pole n m Cnm Snm");
        System.out.println("# A ns ut1Tai xp yp degree order pole x y z ax ay az; total acceleration incl selected monopole");
        System.out.println("# T degree order pole elapsed x y z vx vy vz; MU=3.986004415e14 R=6378136.3, point-mass base");
        long[] epochs={-432000000123456789L,0L,123456789L,315575967816000000L,315575967816000001L,840000000123456789L};
        for(long ns:epochs) {
            var date=EPOCH.shiftedBy(new TimeOffset(ns/1000000000L,(ns%1000000000L)*1000000000L));
            double ut=-31.7,xp=.12*Constants.ARC_SECONDS_TO_RADIANS,yp=.25*Constants.ARC_SECONDS_TO_RADIANS;
            for(int[] cut:new int[][]{{2,0},{2,1},{4,4},{30,12},{100,100}})for(int pole=0;pole<2;pole++) {
                int degree=cut[0],order=cut[1];if(degree>maximum)continue;var f=field(path,degree,order,date,ut,xp,yp,pole==1);
                var h=f.onDate(date);
                for(int[] nm:new int[][]{{2,0},{2,1},{2,2},{4,3},{30,12},{100,100}})if(nm[0]<=degree && nm[1]<=order)
                    System.out.printf("C %d %.17g %.17g %.17g %d %d %d %d %d %.17g %.17g%n",ns,ut,xp,yp,degree,order,pole,nm[0],nm[1],h.getNormalizedCnm(nm[0],nm[1]),nm[1]==0?0.0:h.getNormalizedSnm(nm[0],nm[1]));
                var force=new HolmesFeatherstoneAttractionModel(FRAME,f);
                for(double[] position:new double[][]{{4e6,-3e6,5e6},{.001,-.002,7e6},{26560000,0,0},{0,42164000,0}}) {
                    var r=new Vector3D(position);var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(0,6200,5300)),FRAME,date,MU));
                    var a=force.acceleration(state,new double[]{MU}).add(r.scalarMultiply(-MU/Math.pow(r.getNorm(),3)));
                    System.out.printf("A %d %.17g %.17g %.17g %d %d %d",ns,ut,xp,yp,degree,order,pole);
                    for(double v:position)System.out.printf(" %.17g",v);for(double v:a.toArray())System.out.printf(" %.17g",v);System.out.println();
                }
            }
        }
        for(int[] cut:new int[][]{{4,4},{30,12},{100,100}})for(double dt:new double[]{-86400,-6000,6000,86400}) {
            if(cut[0]>maximum)continue;
            var tide=field(path,cut[0],cut[1],EPOCH,-31.7,.12*Constants.ARC_SECONDS_TO_RADIANS,.25*Constants.ARC_SECONDS_TO_RADIANS,true);
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(new Vector3D(7e6,0,0),new Vector3D(0,6200,5300)),FRAME,EPOCH,MU)));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,tide));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();System.out.printf("T %d %d 1 %.0f",cut[0],cut[1],dt);
            for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);System.out.println();
        }
    }
}
