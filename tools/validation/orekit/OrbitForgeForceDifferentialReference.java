// Independent state partials from unmodified Orekit 13.1.2 force models and
// Hipparchus 4.0.2 Gradient arithmetic. No production derivative formulas.
import java.nio.file.*;
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.forces.gravity.*;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.propagation.FieldSpacecraftState;

public final class OrbitForgeForceDifferentialReference {
    static final StringBuilder OUT=new StringBuilder();
    static final AbsoluteDate EPOCH=OrbitForgeRelativityReference.EPOCH;
    static final Frame FRAME=FramesFactory.getGCRF();
    static void number(double x) {
        if(!Double.isFinite(x))throw new IllegalStateException("nonfinite reference");
        OUT.append(String.format(Locale.ROOT," %.17g",x));
    }
    static void vector(Vector3D v) { for(double x:v.toArray())number(x); }
    static void differential(FieldVector3D<Gradient> vector) {
        for(Gradient g:vector.toArray())number(g.getValue());
        for(int block=0;block<2;block++)for(Gradient g:vector.toArray())
            for(int j=0;j<3;j++)number(g.getGradient()[3*block+j]);
    }
    static FieldSpacecraftState<Gradient> state(Vector3D r,Vector3D v,double t) {
        var field=Gradient.constant(6,0).getField();
        var p=new FieldVector3D<>(Gradient.variable(6,0,r.getX()),Gradient.variable(6,1,r.getY()),Gradient.variable(6,2,r.getZ()));
        var velocity=new FieldVector3D<>(Gradient.variable(6,3,v.getX()),Gradient.variable(6,4,v.getY()),Gradient.variable(6,5,v.getZ()));
        return new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,new FieldAbsoluteDate<>(field,EPOCH.shiftedBy(t)),p,velocity));
    }
    static ExtendedPositionProvider fixed(Vector3D r) {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame) {return r;}
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                return new FieldVector3D<>(date.getField(),r);
            }
        };
    }
    public static void main(String[] args)throws Exception {
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2 Gradient: fixed-epoch state partials, SI inertial axes\n");
        OUT.append("# T: GM body_r[3] r[3] acceleration[3] Jr[9] Jv[9]; both matrices row-major\n");
        OUT.append("# R: EarthGM SunGM r[3] v[3] Sun_r[3] Sun_v[3] pole[3], then a[3] Jr[9] Jv[9] for S,LT,DS,total\n");
        for(boolean isSun:new boolean[]{false,true})for(double t:new double[]{-1e8,0,1e8}) {
            double gm=isSun ? OrbitForgeThirdBodyReference.SUN : OrbitForgeThirdBodyReference.MOON;
            Vector3D rb=OrbitForgeThirdBodyReference.body(isSun).getPosition(EPOCH.shiftedBy(t),FRAME);
            var force=new ThirdBodyAttraction(fixed(rb),"fixed-geometric-body",gm);
            double distance=rb.getNorm();
            for(Vector3D r:new Vector3D[]{Vector3D.ZERO,new Vector3D(1e-6,-2e-6,3e-6),
                new Vector3D(7e6,0,0),new Vector3D(.001,-.002,7e6),new Vector3D(4e6,-3e6,5e6),
                new Vector3D(42164000,2e6,-5e6),rb.add(new Vector3D(.01*distance,-.02*distance,.03*distance))}) {
                var a=force.acceleration(state(r,new Vector3D(100,1000,7000),t),new Gradient[]{Gradient.constant(6,gm)});
                OUT.append('T');number(gm);vector(rb);vector(r);differential(a);OUT.append('\n');
            }
        }
        var schwarz=new Relativity(OrbitForgeRelativityReference.MU);
        var deSitter=new DeSitterRelativity(OrbitForgeRelativityReference.body(true),OrbitForgeRelativityReference.body(false));
        for(double t:new double[]{-1e8,0,1e8})for(double angle:new double[]{0,.2,-.7}) {
            Frame fixed=new Frame(FRAME,new Transform(EPOCH,new Rotation(Vector3D.PLUS_I,angle,RotationConvention.FRAME_TRANSFORM)),"body");
            var lt=new LenseThirringRelativity(OrbitForgeRelativityReference.MU,fixed);
            var pole=fixed.getStaticTransformTo(FRAME,EPOCH).transformVector(Vector3D.PLUS_K);
            for(double[][] pv:new double[][][]{{{7e6,0,0},{0,6200,5300}},{{.001,-.002,7e6},{7300,10,50}},
                {{4e6,-3e6,5e6},{2000,5000,-4000}},{{42164e3,0,0},{0,3074,0}},{{7e6,0,0},{0,0,0}},
                {{7e6,0,0},{1000,0,0}},{{6.5e6,-3e6,2e6},{12000,-2200,3100}}}) {
                var r=new Vector3D(pv[0]);var v=new Vector3D(pv[1]);var state=state(r,v,t);
                var a=schwarz.acceleration(state,new Gradient[]{Gradient.constant(6,OrbitForgeRelativityReference.MU)});
                var b=lt.acceleration(state,new Gradient[]{Gradient.constant(6,OrbitForgeRelativityReference.MU)});
                var c=deSitter.acceleration(state,new Gradient[]{Gradient.constant(6,OrbitForgeRelativityReference.SUN_MU)});
                OUT.append('R');number(OrbitForgeRelativityReference.MU);number(OrbitForgeRelativityReference.SUN_MU);
                for(var x:new Vector3D[]{r,v,OrbitForgeRelativityReference.sunPosition(t),OrbitForgeRelativityReference.sunVelocity(t),pole})vector(x);
                for(var x:List.of(a,b,c,a.add(b).add(c)))differential(x);
                OUT.append('\n');
            }
        }
        Files.writeString(Path.of(args[0]),OUT.toString(),java.nio.charset.StandardCharsets.US_ASCII);
    }
}
