// Orekit 13.1.2 / Hipparchus 4.0.2: independent Cartesian, selected-Sun and K
// automatic derivatives. D uses SolarRadiationPressure (spherical Earth).
// N/C use IsotropicRadiationSingleCoefficient with constant full/zero flux gates.
import java.nio.file.*;
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.frames.*;
import org.orekit.forces.radiation.*;
import org.orekit.propagation.*;
import org.orekit.time.*;
import org.orekit.utils.*;

public final class OrbitForgeSrpDifferentialReference {
    static final int DIM=10;
    static final double AU=149597870700.,P=4.56e-6,EARTH=6378137.;
    static final Frame FRAME=FramesFactory.getGCRF();
    static final AbsoluteDate DATE=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final StringBuilder OUT=new StringBuilder();
    static boolean rawFrame=false;
    static Gradient constant(double x){return Gradient.constant(DIM,x);}
    static FieldVector3D<Gradient> variables(Vector3D v,int offset) {
        return new FieldVector3D<>(Gradient.variable(DIM,offset,v.getX()),Gradient.variable(DIM,offset+1,v.getY()),Gradient.variable(DIM,offset+2,v.getZ()));
    }
    static ExtendedPositionProvider provider(Vector3D sun) {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame){return sun;}
            @SuppressWarnings("unchecked")
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                return new FieldVector3D<>((T)Gradient.variable(DIM,6,sun.getX()),(T)Gradient.variable(DIM,7,sun.getY()),(T)Gradient.variable(DIM,8,sun.getZ()));
            }
        };
    }
    static void number(double x) {
        if(!Double.isFinite(x))throw new IllegalStateException("nonfinite reference");
        OUT.append(String.format(Locale.ROOT," %.17g",x));
    }
    static void vector(Vector3D v) {for(double x:v.toArray())number(x);}
    static void row(char model,Vector3D r,Vector3D sun,double cr,double ratio,double radius,Rotation rotation) {
        // A sphere has a repeated principal axis in Ellipsoid.getPlaneSection.
        // Its field eigendirection can be ill-conditioned after arbitrary rotation.
        // Evaluate unmodified Orekit in a canonical plane, then rotate the scalar,
        // vector and rank-two derivatives. --raw-frame reproduces the upstream issue.
        if(rawFrame){r=rotation.applyTo(r);sun=rotation.applyTo(sun);rotation=Rotation.IDENTITY;}
        var date=new FieldAbsoluteDate<>(constant(0).getField(),DATE);
        var position=variables(r,0);
        var state=new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,date,position,variables(new Vector3D(1234,6700,-2300),3))).withMass(constant(1000));
        var spacecraft=new IsotropicRadiationSingleCoefficient(1000,1);
        var parameters=new Gradient[]{constant(1),Gradient.variable(DIM,9,cr*ratio)};
        FieldVector3D<Gradient> acceleration;
        Gradient lighting;
        if(model=='D') {
            var force=new SolarRadiationPressure(AU,P,provider(sun),new OneAxisEllipsoid(radius,0,FRAME),spacecraft);
            acceleration=force.acceleration(state,parameters);
            lighting=force.getLightingRatio(state);
        } else {
            var toSun=variables(sun,6).subtract(position);
            double f=1;
            if(model=='C') {
                var n=sun.subtract(r).normalize();
                var toEarth=r.negate();
                if(toEarth.dotProduct(n)>0 && toEarth.getNorm()<sun.subtract(r).getNorm() &&
                   Vector3D.crossProduct(toEarth,n).getNorm()<radius)f=0;
            }
            lighting=constant(f);
            var norm=toSun.getNorm();
            var flux=toSun.scalarMultiply(norm.pow(-3).multiply(-P*AU*AU*f));
            acceleration=spacecraft.radiationPressureAcceleration(state,flux,parameters);
        }
        OUT.append(model);number(cr);number(ratio);number(radius);number(Constants.SUN_RADIUS);vector(rotation.applyTo(r));vector(rotation.applyTo(sun));
        number(lighting.getValue());
        for(int offset:new int[]{0,6})vector(rotation.applyTo(new Vector3D(Arrays.copyOfRange(lighting.getGradient(),offset,offset+3))));
        vector(rotation.applyTo(acceleration.toVector3D()));
        double[][] matrix=rotation.getMatrix();
        Gradient[] components=acceleration.toArray();
        for(int offset:new int[]{0,3,6})for(int i=0;i<3;i++)for(int j=0;j<3;j++) {
            double sum=0;
            for(int a=0;a<3;a++)for(int b=0;b<3;b++)sum+=matrix[i][a]*components[a].getGradient()[offset+b]*matrix[j][b];
            number(sum);
        }
        vector(rotation.applyTo(new Vector3D(components[0].getGradient()[9],components[1].getGradient()[9],components[2].getGradient()[9])));
        OUT.append('\n');
    }
    public static void main(String[] args)throws Exception {
        rawFrame=args.length>1 && args[1].equals("--raw-frame");
        OUT.append(rawFrame ? "# Raw field evaluation in supplied axes\n" : "# Unmodified spherical Orekit in canonical plane; output tensors rotated to supplied axes\n");
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2 Gradient: r,v,Sun,K; fixed geometry; SI\n");
        OUT.append("# model Cr A/M EarthRadius SunRadius r[3] Sun[3] f grad_r_f[3] grad_Sun_f[3] a[3] Jr[9] Jv[9] Jsun[9] d_a/d_K[3]\n");
        int index=0;
        for(double radius:new double[]{EARTH,EARTH+23000})for(double height:new double[]{7e6,42164000,4e8,2e9}) {
            double beta=Math.asin(radius/height),alpha=Math.asin(Constants.SUN_RADIUS/(AU+height));
            for(double phi:new double[]{.0003,beta-1.2*alpha,beta-.75*alpha,beta-.25*alpha,beta+.25*alpha,beta+.75*alpha,beta+1.2*alpha,Math.PI-.2})
                for(double tilt:new double[]{0,.3,Math.asin(.8),1.3}) {
                    var rotation=new Rotation(Vector3D.PLUS_J,tilt,RotationConvention.VECTOR_OPERATOR);
                    var r=new Vector3D(-height*Math.cos(phi),height*Math.sin(phi),0);
                    var sun=new Vector3D(AU,0,0);
                    double cr=new double[]{1.7,0,-2.2}[index++%3];
                    row('D',r,sun,cr,.03,radius,rotation);
                }
        }
        for(char model:new char[]{'N','C'})for(double cr:new double[]{1.7,0,-2.2})
            for(Vector3D r:new Vector3D[]{new Vector3D(7e6,1e6,2e6),new Vector3D(-7e6,1e6,2e6),new Vector3D(-7e6,8e6,2e6)})
                row(model,r,new Vector3D(AU,1e8,-2e8),cr,.03,EARTH,Rotation.IDENTITY);
        Files.writeString(Path.of(args[0]),OUT.toString());
    }
}
