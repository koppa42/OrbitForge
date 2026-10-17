// Independent analytic Hessian: unmodified Orekit 13.1.2 Holmes-Featherstone
// and NewtonianAttraction, reached through public Hipparchus Gradient states.
package org.orekit.forces.gravity.potential;

import java.nio.file.*;
import java.util.*;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.forces.gravity.*;
import org.orekit.frames.FramesFactory;
import org.orekit.time.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.utils.FieldPVCoordinates;

public final class OrbitForgeGravityDifferentialReference {
    static final double MU=3.986004415e14, R=6378136.3;
    static final AbsoluteDate EPOCH=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final StringBuilder OUT=new StringBuilder();
    static final double[][] POSITIONS={
        {R+10,0,0},{4e6,-3e6,5e6},{.001,-.002,6356752.3},
        {-.001,.002,-6356752.3},{42164000,2e6,-5e6},{1e6,6.3e6,1e6},
        {(R+10)*Math.sqrt(1-Math.pow(.75-2e-10,2)),0,(R+10)*(.75-2e-10)},
        {(R+10)*Math.sqrt(1-Math.pow(.75+2e-10,2)),0,-(R+10)*(.75+2e-10)}
    };
    static NormalizedSphericalHarmonicsProvider evolving(double years) {
        double[][] c=new double[7][],s=new double[7][];
        for(int n=0;n<=6;n++){c[n]=new double[n+1];s[n]=new double[n+1];}
        c[0][0]=1;
        c[2][0]=-4.84165371736e-4 + (1.2e-9 + years*1.7e-11);
        c[2][1]=3e-10 + (2.1e-9 - years*2.3e-11);
        s[2][1]=-4e-10 + (-1.9e-9 + years*3.1e-11);
        c[2][2]=7e-11 + years*1.3e-11;
        s[2][2]=-5e-11 - years*2.7e-11;
        c[4][1]=-7e-10;s[4][1]=9e-10;
        c[6][5]=2.3e-9;s[6][5]=-4.7e-9;
        return GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.TIDE_FREE,c,s);
    }
    static void cases(int profile,int degree,int order,double years,
                      NormalizedSphericalHarmonicsProvider provider) {
        var frame=FramesFactory.getGCRF();
        var field=Gradient.constant(3,0).getField();
        var mu=Gradient.constant(3,MU);
        var central=new NewtonianAttraction(MU);
        var harmonic=provider==null ? null : new HolmesFeatherstoneAttractionModel(frame,provider);
        for(double[] pos:POSITIONS) {
            var r=new FieldVector3D<>(Gradient.variable(3,0,pos[0]),Gradient.variable(3,1,pos[1]),
                                      Gradient.variable(3,2,pos[2]));
            var v=new FieldVector3D<>(field,new Vector3D(100,1000,7000));
            var state=new FieldSpacecraftState<>(new FieldCartesianOrbit<>(new FieldPVCoordinates<>(r,v),
                frame,new FieldAbsoluteDate<>(field,EPOCH),mu));
            var a=central.acceleration(state,new Gradient[]{mu});
            if(harmonic!=null)a=a.add(harmonic.acceleration(state,new Gradient[]{mu}));
            OUT.append(String.format(Locale.ROOT,"%d %d %d %.17g",profile,degree,order,years));
            for(double x:pos)OUT.append(String.format(Locale.ROOT," %.17g",x));
            for(Gradient x:a.toArray())OUT.append(String.format(Locale.ROOT," %.17g",x.getValue()));
            for(Gradient x:a.toArray())for(double d:x.getGradient()) {
                if(!Double.isFinite(d))throw new IllegalStateException("nonfinite oracle Hessian");
                OUT.append(String.format(Locale.ROOT," %.17g",d));
            }
            OUT.append('\n');
        }
    }
    public static void main(String[] args)throws Exception {
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2: analytic Holmes-Featherstone Hessian plus NewtonianAttraction\n");
        OUT.append("# profile degree order years x y z ax ay az Jxx Jxy Jxz Jyx Jyy Jyz Jzx Jzy Jzz; SI, fixed Earth axes\n");
        OUT.append("# profile 0: dense sin/cos; 2: sparse full-degree boundary terms; 3: secular + additive coefficients beyond base 2x2\n");
        for(int[] size:new int[][]{{0,0},{2,0},{2,2},{4,4},{20,0},{20,20},{360,0},{360,360},
                                   {720,720},{1440,1440},{2190,0},{2190,2159},{2190,2190}}) {
            cases(0,size[0],size[1],0,size[0]==0 ? null :
                  OrbitForgeFullGravityReference.synthetic(0,size[0],size[1]));
        }
        for(int order:new int[]{0,1,2,2159,2190})
            cases(2,2190,order,0,OrbitForgeFullGravityReference.synthetic(2,2190,order));
        for(double years:new double[]{-20,0,12.5})cases(3,2,2,years,evolving(years));
        Files.writeString(Path.of(args[0]),OUT.toString(),java.nio.charset.StandardCharsets.US_ASCII);
    }
}
