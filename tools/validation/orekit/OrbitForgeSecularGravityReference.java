// Independent reference: unmodified Orekit 13.1.2 / Hipparchus 4.0.2.
// This package permits access to Orekit's own package-private secular provider.
// Read explicit synthetic .grv inputs; no OrbitForge reader, force or integrator is used.
package org.orekit.forces.gravity.potential;
import java.nio.file.*;
import java.math.*;
import java.security.*;
import java.util.*;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.*;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.utils.*;

public final class OrbitForgeSecularGravityReference {
    public static final double MU=3.986004415e14, R=6378136.3;
    public static final Frame FRAME=FramesFactory.getGCRF();
    public static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final String[] HASHES={"0a5981a811726d9408b42dd432b14fb664fde81a79c971ae5e71a1d5724ba2b1","a15339e5c9f0856eac88da371157d1e0fcc808ec50aa1a14ac30ecf1c2639380","9780376f5db774fb8c864bbb255f23bddbfb1e7e9618b10b29ed858806f1b67b"};
    static final int[][] COEFFICIENTS={{2,0},{2,1},{3,0},{8,4}};
    static final long YEAR_NS=31557600000000000L;
    public static AbsoluteDate date(long ns) {
        return EPOCH.shiftedBy(ns/1000000000L).shiftedBy((ns%1000000000L)*1e-9);
    }
    public static NormalizedSphericalHarmonicsProvider provider(Path root,int profile,int degree,
                                                                int order,boolean enabled,boolean zero) throws Exception {
        Path path=root.resolve("gravity_secular_"+profile+".grv");
        byte[] bytes=Files.readAllBytes(path);
        if(!HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes)).equals(HASHES[profile]))
            throw new IllegalArgumentException("Reference input hash mismatch");
        Flattener f=new Flattener(degree,order);
        double[] c=new double[f.arraySize()],s=c.clone(),dc=c.clone(),ds=c.clone();
        double[][] factors=GravityFieldFactory.getUnnormalizationFactors(8,4);
        boolean normalized=true,coefficients=false;
        AbsoluteDate reference=null;
        if(!zero)c[f.index(0,0)]=1;
        for(String line:Files.readAllLines(path)) {
            String text=line.trim();
            if(text.isEmpty()||text.startsWith("#"))continue;
            String[] t=text.split("\\s+");
            if(t[0].equals("Normalized"))normalized=t[1].equals("Yes");
            if(t[0].equals("RefEpochTDT")) {
                BigDecimal ns=new BigDecimal(t[1]).subtract(new BigDecimal("2451545"))
                    .multiply(new BigDecimal("86400000000000")).subtract(new BigDecimal("32184000000"));
                reference=date(ns.setScale(0,RoundingMode.HALF_UP).longValueExact());
            }
            if(t[0].equals("LinearRate")) {
                int n=Integer.parseInt(t[2]),m=Integer.parseInt(t[3]);
                if(n<=degree&&m<=order&&enabled) {
                    double value=Double.parseDouble(t[4])/Constants.JULIAN_YEAR;
                    if(!normalized)value/=factors[n][m];
                    (t[1].equals("Cos")?dc:ds)[f.index(n,m)]=value;
                }
            }
            if(text.equals("Begin Coefficients")){coefficients=true;continue;}
            if(text.equals("End Coefficients")){coefficients=false;continue;}
            if(coefficients&&!zero) {
                int n=Integer.parseInt(t[0]),m=Integer.parseInt(t[1]);
                if(n<=degree&&m<=order) {
                    c[f.index(n,m)]=Double.parseDouble(t[2])/(normalized?1:factors[n][m]);
                    s[f.index(n,m)]=Double.parseDouble(t[3])/(normalized?1:factors[n][m]);
                }
            }
        }
        if(reference==null)throw new IllegalArgumentException("Missing reference epoch");
        return new WrappingNormalizedProvider(new SecularTrendSphericalHarmonics(
            new ConstantSphericalHarmonics(R,MU,TideSystem.TIDE_FREE,f,c,s),reference,f,dc,ds));
    }
    public static NumericalPropagator propagator(AbsoluteDate start) {
        var integrator=new DormandPrince853Integrator(.0001,30,
            new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
            new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
        var prop=new NumericalPropagator(integrator);
        prop.setOrbitType(OrbitType.CARTESIAN);
        prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(
            new Vector3D(7e6,0,0),new Vector3D(0,6200,5300)),FRAME,start,MU)));
        return prop;
    }
    static void vector(Vector3D v) { for(double x:v.toArray())System.out.printf(" %.17g",x); }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        Path root=Path.of(args[0]);
        System.out.println("# Orekit 13.1.2 SecularTrendSphericalHarmonics + Holmes-Featherstone; Hipparchus 4.0.2 DP853");
        for(int i=0;i<3;i++)System.out.println("# gravity_secular_"+i+".grv sha256="+HASHES[i]);
        System.out.println("# Explicit file epochs/rates; synthetic fields, not STK defaults/output; identity body frame");
        System.out.println("# C file epochTAIns n m deltaC deltaS");
        System.out.println("# A file epochTAIns degree order enabled x y z ax ay az");
        System.out.println("# T file epochTAIns degree order enabled elapsed x y z vx vy vz");
        long[] epochs={-100*YEAR_NS,0,26*YEAR_NS,100*YEAR_NS};
        Vector3D[] positions={new Vector3D(7e6,0,0),new Vector3D(4e6,-3e6,5e6),
                             new Vector3D(.001,-.002,7e6),new Vector3D(4.2e7,1e7,-2e7)};
        for(int file=0;file<3;file++) {
            var changes=provider(root,file,8,4,true,true);
            for(long epoch:epochs) {
                var h=changes.onDate(date(epoch));
                for(int[] nm:COEFFICIENTS)System.out.printf("C %d %d %d %d %.17g %.17g%n",
                    file,epoch,nm[0],nm[1],h.getNormalizedCnm(nm[0],nm[1]),h.getNormalizedSnm(nm[0],nm[1]));
                for(int[] cut:new int[][]{{2,0},{2,1},{8,4}})for(int enabled=0;enabled<2;enabled++) {
                    var force=new HolmesFeatherstoneAttractionModel(FRAME,provider(root,file,cut[0],cut[1],enabled!=0,false));
                    for(Vector3D r:positions) {
                        var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(0,6200,5300)),FRAME,date(epoch),MU));
                        var accel=force.acceleration(state,new double[]{MU})
                            .add(new NewtonianAttraction(MU).acceleration(state,new double[]{MU}));
                        System.out.printf("A %d %d %d %d %d",file,epoch,cut[0],cut[1],enabled);
                        vector(r);vector(accel);System.out.println();
                    }
                }
            }
            for(long epoch:new long[]{0,26*YEAR_NS})for(int[] cut:new int[][]{{2,0},{8,4}})
                for(int enabled=0;enabled<2;enabled++)for(double dt:new double[]{-86400,86400}) {
                    var prop=propagator(date(epoch));
                    prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,provider(root,file,cut[0],cut[1],enabled!=0,false)));
                    var pv=prop.propagate(date(epoch).shiftedBy(dt)).getPVCoordinates();
                    System.out.printf("T %d %d %d %d %d %.0f",file,epoch,cut[0],cut[1],enabled,dt);
                    vector(pv.getPosition());vector(pv.getVelocity());System.out.println();
                }
        }
    }
}
