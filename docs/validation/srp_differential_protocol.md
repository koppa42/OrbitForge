# 太阳辐射压偏导验证

契约见[SRP 局部偏导](../architecture/srp_differential.md)。固定参考离线生成，测试不联网、不依赖 Java、SciPy 或 mpmath，也不从 OrbitForge 输出生成期望值。

## 三套独立参考

| 固定输入 | 方法及范围 |
| --- | --- |
| `srp_differential.txt`，274 组 | 固定 Orekit 13.1.2 / Hipparchus 4.0.2，十变量 `Gradient` 同时求 r、v、所选太阳和 K 的偏导。256 组球形地球 `SolarRadiationPressure`，另 18 组无阴影／圆柱常值分支使用 `IsotropicRadiationSingleCoefficient`；圆柱门限独立判定，不声称运行了 Orekit 的圆柱方向模型。 |
| `srp_differential_high.txt`，256 组 | 与上表双锥部分使用相同物理输入；mpmath 1.3.0 在 60/80 位精度下计算独立两圆透镜面积公式，用 `mpmath.diff` 求六个几何变量的照明与完整力偏导。不使用生产圆弧算法或 Orekit 期望列。 |
| `srp_union_differential.txt`，111 组 | SciPy 1.16.3 QUADPACK 垂直切片积分求多圆交并面积，独立笛卡尔八阶中心差分求照明与完整力 Jacobian。包括 0…10 个遮挡体、同心、物理重复、包含、分离、六体环状、随机交叠和地球加九体，三种空间姿态。 |

单球覆盖 7e6、42164000、4e8、2e9 m 地心距离，0／23 km 等效大气高度，全照／半影／本影／环食及四种坐标旋转。Cr 循环使用 1.7、0、−2.2，A/m=0.03 m²/kg；太阳参考距离 149597870700 m、半径 695700000 m、压力 4.56e−6 Pa。Orekit 质量与面积均设为 1000，辐射系数变量设为 K，从而验证 STK 的 K 偏导。太阳为显式独立几何变量，不混入不同星历或时标。

多体参考保持各球体物理中心固定后扰动卫星／太阳。初始细差分步长为位置 512 m、太阳 16384 m，与二倍步长比较；不能满足收敛门限时逐次减半相应步长，最多十次。文件保存每个样本实际步长，接触附近不能使用跨分支的大模板。两档积分容差为 2e−13 / 1e−12，照明最大变化 2.887e−15；照明梯度收敛误差分别不超过约 8.430e−9 / 2.668e−8（参考收敛分别按 `||g||∞ + 4e−10 m⁻¹` / `||g||∞ + 2e−12 m⁻¹` 归一化）。

## 参考库的数值限制

原始 Orekit `Ellipsoid.getPlaneSection` 对球体具有重复主轴。任意旋转后，截面特征方向的自动微分可能病态；本组原坐标直接求值相对高精度照明梯度最大偏差超过 2000 倍，不能当作真值。生成器默认在共面标准坐标中调用**未修改的 Orekit**，再按旋转不变性变换梯度及 `R J Rᵀ`；输出仍覆盖全部姿态。附加 `--raw-frame` 可复现直接求值差异。[固定版本截面实现](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/bodies/Ellipsoid.java)

此外，该版本环食分支将反余弦参数夹到 `nextDown(1)`，照明和梯度有微小偏置；高精度审计测得照明约 2.453e−9、梯度约 4.744e−9。Orekit 对照门限包含此独立测得的参考误差，同时保留更严格的高精度测试，避免放宽生产实现验收。[固定版本光压实现](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/forces/radiation/SolarRadiationPressure.java)

高精度公式是独立模型计算，不是 80 位 Orekit 或 STK。60/80 位所有输出最大差 1.226e−54。当前没有 STK 安装与同配置数值结果。

## 验收门限与结果

照明梯度分别按对应三维无穷范数比较；力及 K 偏导的绝对门限由**未遮挡**的压力／力尺度乘照明门限得到，避免半影中的近零名义力造成不合理的相对误差。

- Orekit：照明绝对 3e−9，梯度及状态块相对 8e−9；小量底限明确写在测试中。
- 独立高精度：照明绝对 1e−11，梯度及状态块相对 1e−10；照明最大误差 2.302e−12，位置／太阳照明梯度最大归一化误差约 1.420e−11。
- 多体：照明绝对 2e−11；位置梯度每分量 `1e−7 ||g||∞ + 2e−17 m⁻¹`，太阳梯度为 `1e−7 ||g||∞ + 1e−19 m⁻¹`。力块采用相同相对门限和压力尺度换算的底限。参考自身使用相对 5e−8、相同底限的收敛门限。
- 多体实际照明最大误差 4.885e−15；按 `||g||∞ + 2e−10 m⁻¹`／`||g||∞ + 1e−12 m⁻¹` 归一化的最大梯度误差为 8.427e−9 / 2.370e−8。
- 全部速度块在固定几何下严格为零。另检验零／负 Cr、零面积、禁用、缺失和非法几何、不可微接触拒绝、重复物理球的等价性及列表重排不改变结果。

以上数值验证实现误差，不能代表球面光压模型的物理精度或完整协方差精度。

## 重现

Orekit 依赖及哈希位于 `tools/validation/orekit/dependencies.json`。编译 `OrbitForgeSrpDifferentialReference.java`，依次生成标准与原坐标候选：

```text
java -cp <classes-and-pinned-jars> OrbitForgeSrpDifferentialReference <orekit.txt>
java -cp <classes-and-pinned-jars> OrbitForgeSrpDifferentialReference <raw.txt> --raw-frame
python tools/validation/srp_sphere_differential_reference.py --input <orekit.txt> --output <high.txt> --raw-orekit <raw.txt>
python tools/validation/srp_union_differential_reference.py --output <union.txt>
```

高精度生成器同时输出 Orekit 两种求值方式的审计结果；多体生成器必须通过逐样本收敛检查才写出文件。重复生成并核对候选字节，再审阅更新固定输入。C++ 入口为 `SrpDifferential.*`，名义回归为 `SolarRadiationPressure.*` / `SrpBoundary.*`，安装 Python 验证 `test_srp.py` 与 HPOP，SDK 消费端实际调用新增 C++ 接口。

| 固定文件 | SHA-256（LF） |
| --- | --- |
| srp_differential.txt | `72ac9ca4c55cd4f0e7396d02f3f45e2c3c708f3de1d6f2e7e2083c25c43c1db9` |
| srp_differential_high.txt | `863ed9456fe77de75eb16071c326e6b87e8a80af9f91e8eedbb46d06d8f8e078` |
| srp_union_differential.txt | `1e964559601120bf8a40434b33c08d905c8649bce668afc9c6aeaebcd11cee3d` |
