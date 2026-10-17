# 中心引力空间偏导验证

接口与模型约定见[中心引力空间偏导](../architecture/gravity_differential.md)。参考文件保存输入位置、加速度和行优先 3×3 Jacobian，均为 SI、固定地心轴；不涉及地球定向和时标转换误差。以下容限衡量对齐模型下的算法差异，不能当成真实地球重力误差或长期轨道精度。

## 独立参考

### Orekit 解析 Hessian

`tools/validation/orekit/OrbitForgeGravityDifferentialReference.java` 使用未修改的 Orekit **13.1.2** / Hipparchus **4.0.2**。生产二阶 Jet 不参与参考生成；参考通过公共 `FieldSpacecraftState<Gradient>`、`HolmesFeatherstoneAttractionModel.acceleration` 得到 Orekit 自己的解析 Hessian，再叠加 `NewtonianAttraction`。版本及八个 JAR 的 SHA-256 固定在 `tools/validation/orekit/dependencies.json`。

复用 `OrbitForgeFullGravityReference.synthetic` 的系数定义，168 组涵盖：

- 0、2、4、20、360、720、1440、2190 阶，零阶次、全阶次及 2190×2159；
- 密集 sin/cos 合成场，及 C20 加放大的 2190 阶 zonal / tesseral / sectorial 边界项；
- 赤道、LEO、GEO、南北极轴旁 2.24 mm，以及生产极区分支两侧；
- 2×2 底场、−20 / 0 / 12.5 儒略年演化，以及扩展至 6×5 的加性潮汐形状系数。这里检查系数叠加，不替代真实潮汐时间模型的独立验证。

`tests/reference/propagators/gravity_differential.txt`：

```text
SHA-256 014aa617884e115dd250208758cde4e72a4182b28a4994ad367509e389cb82a7
```

逐分量加速度门限为 `5e−11 m/s²`，Hessian 门限为 `8e−12 max|H_ref| + 3e−19 s⁻²`。Orekit 球坐标到 Cartesian 的 Hessian 在极近轴、放大的 2190 阶场中有约 `4e−12` 的相对零迹残差，所以另用下述高精度极轴真值作更紧的验证，不以二者一致作为唯一证据。

### 80 位精度的极轴解析值

`tools/validation/gravity_differential_poles.py` 只用 Python 标准库 Decimal，精度 80 位。它依据 `P_n(±1)`、`P'_n(±1)`、`P''_n(±1)` 及势函数的调和性，直接计算极轴上的加速度和 Hessian，不调用生产 Cartesian 递推或 OrbitForge 模块。binary64 输入通过 `Decimal.from_float` 精确保留。

在精确轴上只有 m=0、1、2 能贡献二阶导数。76 组覆盖 2、20、360、2190 阶、不同独立次数、密集／放大边界系数场、南北极以及两种半径。高阶 m>2 项也存在于被测模型中，应在轴上自然给出零贡献。

`tests/reference/propagators/gravity_differential_poles.txt`：

```text
SHA-256 cef3d0c52b89abbe566f831500f405590f169e824c3ec355eef566cd52432cae
```

逐分量加速度门限为 `5e−12 m/s²`，Hessian 为 `2e−13 max|H_ref| + 2e−21 s⁻²`。精确对称性和零迹同时检查，零迹门限为 `2e−13 max|H| + 2e−21 s⁻²`；生产实现没有人为把迹设成零。

## 复现

先按清单下载并逐项校验 JAR，将下面的 `<classpath>` 替换为这些 JAR 的平台类路径；所有输出放入新的 `out/validation/` 子目录：

```text
javac -cp <classpath> -d <classes> tools/validation/orekit/OrbitForgeFullGravityReference.java tools/validation/orekit/OrbitForgeGravityDifferentialReference.java
java -Xmx2g -cp <classes-and-jars> org.orekit.forces.gravity.potential.OrbitForgeGravityDifferentialReference <output.txt>
python tools/validation/gravity_differential_poles.py <poles.txt>
```

对每个生成器重复运行，核对原始 LF 字节和上述 SHA-256。CMake 已将固定数据注册到核心测试：

```text
orbitforge_core_tests --gtest_filter=GravityDifferential.*:Gravity.*
```

每组参考另外要求 `differential().acceleration_mps2` 与普通 `acceleration()` 逐位相同。核心测试还覆盖单极闭式 Jacobian、非法阶次／位置／系数／缺失长期变化元数据、底场外加性项、计算中取消和共享不可变模型的并发历元。SDK 消费端从安装目录实际调用新方法。

变更球谐递推时运行完整 Core 和相关重力／潮汐／HPOP 集成回归。对普通力路径的性能检查应使用同一工具链、参数和机器，编译变更前后两个版本，记录预热、重复计时及输出校验；不要将带 Hessian 的调用开销算到未启用偏导的传播中。
