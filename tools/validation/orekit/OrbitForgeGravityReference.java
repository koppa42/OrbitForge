// Independent high-degree/polar reference; no OrbitForge algorithms or libraries.
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.orekit.forces.gravity.HolmesFeatherstoneAttractionModel;
import org.orekit.forces.gravity.potential.GravityFieldFactory;
import org.orekit.forces.gravity.potential.TideSystem;
import org.orekit.frames.FramesFactory;
import org.orekit.time.AbsoluteDate;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.propagation.SpacecraftState;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeGravityReference {
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        double mu=3.986004415e14, radius=6378136.3;
        System.out.println("# Orekit 13.1.2 Holmes-Featherstone; synthetic fully normalized Earth field");
        System.out.println("# C20=-4.84165371736e-4; other C/S=1e-8/(n+1)^2*sin/cos((n+1)*(m+2)); S(n,0)=0");
        System.out.println("# degree order x y z ax ay az; SI");
        for (int degree : new int[]{21,70,180,360}) for (int order : new int[]{0,degree}) {
            double[][] c=new double[degree+1][], s=new double[degree+1][];
            for(int n=0;n<=degree;n++) {
                c[n]=new double[Math.min(n,order)+1]; s[n]=new double[c[n].length];
                if(n<2) continue;
                for(int m=0;m<c[n].length;m++) {
                    c[n][m]=(n==2 && m==0) ? -4.84165371736e-4 : 1e-8/((n+1.0)*(n+1))*Math.sin((n+1.0)*(m+2));
                    s[n][m]=m==0 ? 0 : 1e-8/((n+1.0)*(n+1))*Math.cos((n+1.0)*(m+2));
                }
            }
            c[0][0]=1;
            var provider=GravityFieldFactory.getNormalizedProvider(radius,mu,TideSystem.TIDE_FREE,c,s);
            var gravity=new HolmesFeatherstoneAttractionModel(FramesFactory.getGCRF(),provider);
            for(double[] pos : new double[][]{{6388136.3,0,0},{4e6,-3e6,5e6},{.001,-.002,7e6},{-.001,.002,-7e6},{42164000,2e6,-5e6}}) {
                var r=new Vector3D(pos);
                var state=new SpacecraftState(new CartesianOrbit(new PVCoordinates(r,new Vector3D(100,1000,7000)),
                    FramesFactory.getGCRF(),AbsoluteDate.J2000_EPOCH,mu));
                var a=gravity.acceleration(state,new double[]{mu})
                    .add(r.scalarMultiply(-mu/Math.pow(r.getNorm(),3)));
                System.out.printf("%d %d",degree,order);
                for(double v:pos) System.out.printf(" %.17g",v);
                for(double v:a.toArray()) System.out.printf(" %.17g",v);
                System.out.println();
            }
        }
    }
}
