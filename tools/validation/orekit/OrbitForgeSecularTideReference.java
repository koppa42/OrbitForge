// Independent three-force composition, using the existing fixed tide geometry.
package org.orekit.forces.gravity;
import java.nio.file.Path;
import java.util.Locale;
import org.orekit.forces.gravity.potential.OrbitForgeSecularGravityReference;
import org.orekit.utils.Constants;

public final class OrbitForgeSecularTideReference {
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        Path root=Path.of(args[0]);
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2: independent secular, full solid and ocean force composition");
        System.out.println("# Secular fields 0/1 from gravity_secular_*.grv; same pinned hashes as gravity_secular.txt");
        System.out.println("# IERS2003 solid tides untruncated, frequency/pole off; IERS2010 FES4x4 including ocean pole");
        System.out.println("# 36 explicit waves, fixed body frame, UT1-TAI=-31.7s, xp=.12 arcsec, yp=.25 arcsec");
        System.out.println("# X file baseDegree baseOrder elapsed x y z vx vy vz; initial epoch=0 TAI ns");
        for(int profile:new int[]{0,1})for(int[] cut:new int[][]{{2,0},{8,4}})
            for(double dt:new double[]{-86400,86400}) {
                var prop=OrbitForgeSecularGravityReference.propagator(OrbitForgeOceanTideReference.EPOCH);
                prop.addForceModel(new HolmesFeatherstoneAttractionModel(OrbitForgeOceanTideReference.FRAME,
                    OrbitForgeSecularGravityReference.provider(root,profile,cut[0],cut[1],true,false)));
                prop.addForceModel(new HolmesFeatherstoneAttractionModel(OrbitForgeOceanTideReference.FRAME,
                    OrbitForgeSolidTideReference.tides(false,0)));
                var ocean=OrbitForgeOceanTideReference.field(root.resolve("ocean_fes2014bv1_4x4.pot"),4,4,
                    OrbitForgeOceanTideReference.EPOCH,-31.7,.12*Constants.ARC_SECONDS_TO_RADIANS,
                    .25*Constants.ARC_SECONDS_TO_RADIANS,true);
                prop.addForceModel(new HolmesFeatherstoneAttractionModel(OrbitForgeOceanTideReference.FRAME,ocean));
                var pv=prop.propagate(OrbitForgeOceanTideReference.EPOCH.shiftedBy(dt)).getPVCoordinates();
                System.out.printf("X %d %d %d %.0f",profile,cut[0],cut[1],dt);
                for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
                for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
                System.out.println();
            }
    }
}
