// Independent Orekit 13.1.2 NRLMSISE00 / DragForce, using Hipparchus 4.0.2
// automatic differentiation (density finite differences explicitly disabled).
import java.nio.file.*;
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.bodies.*;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.forces.drag.*;
import org.orekit.models.earth.atmosphere.*;
import org.orekit.propagation.FieldSpacecraftState;

public final class OrbitForgeDragDifferentialReference {
    static final Frame FRAME=FramesFactory.getGCRF();
    static final AbsoluteDate DATE=new AbsoluteDate(2000,2,29,13,23,12.5,TimeScalesFactory.getTAI());
    static final Vector3D SUN=new Vector3D(1.4e11,-5e10,3e10);
    static final double A=6378137, F=1/298.257223563, E2=F*(2-F), B=A*(1-F);
    static final StringBuilder OUT=new StringBuilder();
    static Gradient constant(double x) {return Gradient.constant(7,x);}
    static void number(double x) {
        if(!Double.isFinite(x))throw new IllegalStateException("nonfinite reference");
        OUT.append(String.format(Locale.ROOT," %.17g",x));
    }
    static void vector(Vector3D v) {for(double x:v.toArray())number(x);}
    static ExtendedPositionProvider sun() {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame) {return SUN;}
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                return new FieldVector3D<>(date.getField(),SUN);
            }
        };
    }
    // The density model remains Orekit's unmodified NRLMSISE00. Only the optional
    // STK radial height convention is supplied through the BodyShape interface.
    static class RadialHeight extends OneAxisEllipsoid {
        RadialHeight() {super(A,F,FRAME);}
        @Override public GeodeticPoint transform(Vector3D p,Frame frame,AbsoluteDate date) {
            var g=super.transform(p,frame,date);
            double q=(p.getX()*p.getX()+p.getY()*p.getY())/p.getNormSq();
            return new GeodeticPoint(g.getLatitude(),g.getLongitude(),p.getNorm()-B/Math.sqrt(1-E2*q));
        }
        @Override public <T extends CalculusFieldElement<T>> FieldGeodeticPoint<T> transform(
            FieldVector3D<T> p,Frame frame,FieldAbsoluteDate<T> date) {
            var g=super.transform(p,frame,date);
            T q=p.getX().square().add(p.getY().square()).divide(p.getNormSq());
            T h=p.getNorm().subtract(q.multiply(-E2).add(1).sqrt().reciprocal().multiply(B));
            return new FieldGeodeticPoint<>(g.getLatitude(),g.getLongitude(),h);
        }
    }
    public static void main(String[] args)throws Exception {
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2 Gradient, NRLMSISE gtd7d; fixed co-rotating axes\n");
        OUT.append("# UTC 2000-02-29 13:23:12.5 (synthetic TAI), Sun=(1.4e11,-5e10,3e10) m\n");
        OUT.append("# mode approx Cd A/M r[3] v[3] rho grad_rho[3] a[3] Jr[9] Jv[9] d_a/d_D[3]\n");
        var ellipsoid=new OneAxisEllipsoid(A,F,FRAME);
        int index=0;
        for(int mode=0;mode<3;mode++)for(int approximate=0;approximate<2;approximate++) {
            final int m=mode;
            double[] ap=m==2?new double[]{7,14,91,110,41,38,23}:new double[]{m==0?15:37.25,0,0,0,0,0,0};
            var parameters=new NRLMSISE00InputParameters() {
                public AbsoluteDate getMinDate(){return AbsoluteDate.PAST_INFINITY;}
                public AbsoluteDate getMaxDate(){return AbsoluteDate.FUTURE_INFINITY;}
                public double getDailyFlux(AbsoluteDate date){return new double[]{150,180,210}[m];}
                public double getAverageFlux(AbsoluteDate date){return new double[]{150,165,160}[m];}
                public double[] getAp(AbsoluteDate date){return ap.clone();}
            };
            var atmosphere=new NRLMSISE00(parameters,sun(),approximate==0?ellipsoid:new RadialHeight(),TimeScalesFactory.getTAI());
            if(mode==2)atmosphere=atmosphere.withSwitch(9,-1);
            var drag=new DragForce(atmosphere,new IsotropicDrag(1000,1),false);
            for(double altitude:new double[]{0,1000,30000,60000,80000,105000,140000,199990,200010,400000,700000,999000,1000000})
                for(double[] geometry:new double[][]{{-80,.4},{0,.7},{45,-2},{80,Math.PI-1e-7},{-45,-Math.PI+1e-7},{89.99,2.3},{-.0001,.7},{.0001,.7},{45,Math.PI+Math.atan2(SUN.getY(),SUN.getX())-1e-7},{45,Math.PI+Math.atan2(SUN.getY(),SUN.getX())+1e-7},{0,0}}) {
                    boolean endpoint=altitude==0 || altitude==1000000;
                    if(endpoint != (geometry[0]==0 && geometry[1]==0))continue;
                    Vector3D r=ellipsoid.transform(new GeodeticPoint(Math.toRadians(geometry[0]),geometry[1],altitude));
                    Vector3D v=new Vector3D(1234,-6234,3456);
                    double cd=new double[]{2.2,0,-1.5,3}[index++%4],ratio=.02;
                    var p=new FieldVector3D<>(Gradient.variable(7,0,r.getX()),Gradient.variable(7,1,r.getY()),Gradient.variable(7,2,r.getZ()));
                    var velocity=new FieldVector3D<>(Gradient.variable(7,3,v.getX()),Gradient.variable(7,4,v.getY()),Gradient.variable(7,5,v.getZ()));
                    var date=new FieldAbsoluteDate<>(constant(0).getField(),DATE);
                    var state=new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,date,p,velocity)).withMass(constant(1000));
                    Gradient rho=atmosphere.getDensity(date,p,FRAME);
                    var force=drag.acceleration(state,new Gradient[]{constant(1),Gradient.variable(7,6,cd*ratio)});
                    OUT.append(mode).append(' ').append(approximate);number(cd);number(ratio);vector(r);vector(v);
                    number(rho.getValue());for(int j=0;j<3;j++)number(rho.getGradient()[j]);
                    for(Gradient g:force.toArray())number(g.getValue());
                    for(int block=0;block<2;block++)for(Gradient g:force.toArray())
                        for(int j=0;j<3;j++)number(g.getGradient()[3*block+j]);
                    for(Gradient g:force.toArray())number(g.getGradient()[6]);
                    OUT.append('\n');
                }
        }
        Files.writeString(Path.of(args[0]),OUT.toString());
    }
}
