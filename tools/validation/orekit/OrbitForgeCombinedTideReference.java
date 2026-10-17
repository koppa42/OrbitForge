// Independent composition check. Compile with the OceanTide and SolidTide generators.
// Orekit applies the two forces separately; no OrbitForge coefficient merge is used.
package org.orekit.forces.gravity;
import static org.orekit.forces.gravity.OrbitForgeOceanTideReference.*;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.utils.*;

public final class OrbitForgeCombinedTideReference {
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        var path=Path.of(args[0]);
        var hash=verifyInput(path,4);
        System.out.println("# Independent Orekit 13.1.2 SolidTidesField + OceanTidesField; Hipparchus 4.0.2 DP853");
        System.out.println("# FES4x4 sha256="+hash+"; IERS_2010 ocean with automatic pole; 36 explicit waves");
        System.out.println("# Full IERS_2003 solid tides untruncated, frequency/pole off, moving Sun/Moon geometry 0");
        System.out.println("# Identity body frame, UTC=TAI-32 s, UT1-TAI=-31.7 s, xp=.12 arcsec, yp=.25 arcsec");
        System.out.println("# T ocean-degree ocean-order pole elapsed x y z vx vy vz; point-mass base, MU=3.986004415e14 R=6378136.3");
        for(int[] cut:new int[][]{{2,0},{4,4}})for(double dt:new double[]{-86400,-6000,6000,86400}) {
            var ocean=field(path,cut[0],cut[1],EPOCH,-31.7,.12*Constants.ARC_SECONDS_TO_RADIANS,
                            .25*Constants.ARC_SECONDS_TO_RADIANS,true);
            var integrator=new DormandPrince853Integrator(.0001,30,
                new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
            var prop=new NumericalPropagator(integrator);
            prop.setOrbitType(OrbitType.CARTESIAN);
            prop.setInitialState(new SpacecraftState(new CartesianOrbit(new PVCoordinates(
                new Vector3D(7e6,0,0),new Vector3D(0,6200,5300)),FRAME,EPOCH,MU)));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,ocean));
            prop.addForceModel(new HolmesFeatherstoneAttractionModel(FRAME,OrbitForgeSolidTideReference.tides(false,0)));
            var pv=prop.propagate(EPOCH.shiftedBy(dt)).getPVCoordinates();
            System.out.printf("T %d %d 1 %.0f",cut[0],cut[1],dt);
            for(double v:pv.getPosition().toArray())System.out.printf(" %.17g",v);
            for(double v:pv.getVelocity().toArray())System.out.printf(" %.17g",v);
            System.out.println();
        }
    }
}
