// Independent full-degree oracle: unmodified Orekit 13.1.2 Holmes-Featherstone/DP853.
// Package access wraps Orekit's ICGEM reader; no production force or integrator code.
package org.orekit.forces.gravity.potential;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.HolmesFeatherstoneAttractionModel;
import org.orekit.frames.FramesFactory;
import org.orekit.time.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeFullGravityReference {
    static final double MU=3.986004415e14, R=6378136.3;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static NormalizedSphericalHarmonicsProvider synthetic(int profile,int degree,int order) {
        double[][] c=new double[degree+1][],s=new double[degree+1][];
        for(int n=0;n<=degree;n++) {
            c[n]=new double[Math.min(n,order)+1];s[n]=new double[c[n].length];
            for(int m=0;n>=2 && m<c[n].length;m++) {
                double a=1e-8/((n+1.0)*(n+1));
                c[n][m]=profile==2 ? 0 : a*Math.sin((n+1.0)*(m+2));
                s[n][m]=profile==2 || m==0 ? 0 : a*Math.cos((n+1.0)*(m+2));
                if(n==2 && m==0)c[n][m]=-4.84165371736e-4;
                if(profile==2 && n==2190) {
                    if(m==0)c[n][m]=1e-8;
                    if(m==1){c[n][m]=1e-10;s[n][m]=-2e-10;}
                    if(m==2){c[n][m]=-2e-10;s[n][m]=3e-10;}
                    if(m==2159){c[n][m]=2e-8;s[n][m]=-3e-8;}
                    if(m==2190){c[n][m]=1e-8;s[n][m]=4e-8;}
                }
            }
        }
        c[0][0]=1;
        return GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.TIDE_FREE,c,s);
    }
    static SpacecraftState initial() {
        return new SpacecraftState(new CartesianOrbit(new PVCoordinates(new Vector3D(R+30000,0,0),
            new Vector3D(0,6200,4700)),FramesFactory.getGCRF(),EPOCH,MU));
    }
    static void acceleration(int profile,int degree,int order,NormalizedSphericalHarmonicsProvider provider) {
        var force=new HolmesFeatherstoneAttractionModel(FramesFactory.getGCRF(),provider);
        for(double[] pos:new double[][]{{R+10,0,0},{4e6,-3e6,5e6},{.001,-.002,6356752.3},
            {-.001,.002,-6356752.3},{42164000,2e6,-5e6},{1e6,6.3e6,1e6},
            {(R+10)*Math.sqrt(1-Math.pow(.75-2e-10,2)),0,(R+10)*(.75-2e-10)},
            {(R+10)*Math.sqrt(1-Math.pow(.75+2e-10,2)),0,-(R+10)*(.75+2e-10)}}) {
            var r=new Vector3D(pos);
            var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(100,1000,7000)),
                FramesFactory.getGCRF(),EPOCH,MU));
            var a=force.acceleration(state,new double[]{MU}).add(r.scalarMultiply(-MU/Math.pow(r.getNorm(),3)));
            System.out.printf("A %d %d %d",profile,degree,order);
            for(double v:pos)System.out.printf(" %.17g",v);
            for(double v:a.toArray())System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
    static void trajectory(int profile,int order,NormalizedSphericalHarmonicsProvider provider) {
        for(double dt:new double[]{-120,120}) {
            var integrator=new DormandPrince853Integrator(.00001,10,
                new double[]{1e-7,1e-7,1e-7,1e-10,1e-10,1e-10,1e-7},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);
            prop.setOrbitType(OrbitType.CARTESIAN);prop.setInitialState(initial());
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FramesFactory.getGCRF(),provider));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %d 2190 %d %.0f",profile,order,dt);
            for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
            for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 Holmes-Featherstone and Hipparchus 4.0.2 DP853; SI; fixed GCRF-aligned Earth");
        System.out.println("# A: profile degree order x y z ax ay az; T: profile degree order elapsed rx ry rz vx vy vz");
        System.out.println("# Profiles 0/1: dense sin/cos field; 1 max order=2159; 2: C20 plus amplified boundary coefficients; 3: EGM2008");
        System.out.println("# Trajectory: epoch=2000-01-01T12:00:00 TAI; r=(6408136.3,0,0); v=(0,6200,4700); no other forces");
        if(args.length==1) {
            var path=Path.of(args[0]);var digest=MessageDigest.getInstance("SHA-256");
            try(var stream=Files.newInputStream(path)) {
                byte[] buffer=new byte[65536];int n;
                while((n=stream.read(buffer))!=-1)digest.update(buffer,0,n);
            }
            String hash=HexFormat.of().formatHex(digest.digest());
            if(!hash.equals("ab5b524da073e63b5bdceb7ca47a0de07a26dd44a1c5798f39fc98dc80af70fd"))throw new IOException("EGM2008 hash mismatch");
            System.out.println("# EGM2008 complete ICGEM source SHA256 "+hash);
            var reader=new ICGEMFormatReader("EGM2008.gfc",false,TimeScalesFactory.getTT());
            reader.setMaxParseDegree(2190);reader.setMaxParseOrder(2159);
            try(var stream=Files.newInputStream(path)){reader.loadData(stream,"EGM2008.gfc");}
            for(int degree:new int[]{20,360,720,1440,2190})for(int order:new int[]{0,Math.min(degree,2159)})
                acceleration(3,degree,order,new WrappingNormalizedProvider(reader.getProvider(true,degree,order)));
            trajectory(3,2159,new WrappingNormalizedProvider(reader.getProvider(true,2190,2159)));
        } else {
            for(int profile=0;profile<2;profile++)for(int degree:new int[]{720,1440,2190})
                for(int order:new int[]{0,Math.min(degree,profile==1 ? 2159 : 2190)})
                    acceleration(profile,degree,order,synthetic(profile,degree,order));
            for(int order:new int[]{0,2159,2190})acceleration(2,2190,order,synthetic(2,2190,order));
            trajectory(1,2159,synthetic(1,2190,2159));
        }
    }
}
