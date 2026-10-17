# 科学插值验证协议

实现与 STK 参数语义见[科学轨道星历插值](../architecture/orbit_interpolation.md)。本协议验证显式表、HPOP 节点生成、普通项目与报告路由、任务序列分段，以及 C++ / Python Session 和桌面配置；VOP 已有独立算法对照；STK 同配置结果尚未实测验收。

## 独立参考

`tools/validation/orekit/OrbitForgeInterpolationReference.java` 只调用固定 Hipparchus 4.0.2 的 `PolynomialFunctionLagrangeForm` 和 `HermiteInterpolator`，后者用 `UnivariateDerivative1` 同时提取函数值与导数。生成器不调用 OrbitForge。所需 `hipparchus-core-4.0.2.jar` 的来源与 SHA-256 固定在 `tools/validation/orekit/dependencies.json`，不增加生产依赖。

```text
javac -cp <jars>/hipparchus-core-4.0.2.jar -d <classes> tools/validation/orekit/OrbitForgeInterpolationReference.java
java -cp <jars>/hipparchus-core-4.0.2.jar;<classes> OrbitForgeInterpolationReference <output.txt>
```

Linux 将 classpath 分隔符 `;` 换为 `:`。先核验 JAR SHA-256，再生成两次，核对逐字节相同。输出使用 ASCII/LF，固定输入为 `tests/reference/propagators/orbit_interpolation.txt`，SHA-256 `8d8122d657c6ba438690e617c9e3ee22bd46a60207b22c42a818c87b6d6162fa`。

118 组 / 826 查询覆盖 Lagrange 0–29 和所有 Hermite 奇数 3–29，等间隔及交替 17/23 秒节点、节点命中、端点内侧 1 ns 和非整秒。输入包含倾斜圆轨道以及刻意与位置导数不一致的独立速度数据，后者仅用于验证 Lagrange 的独立速度语义。参考保存完整节点和预期状态，Core 与安装后的 Python 直接读取同一输入；统一加上 `8e18` TAI 纳秒的绝对历元，以验证先作时间差。方法一致性容限逐分量为位置 `2e-5 m`、速度 `2e-7 m/s`；这些不是低阶插值相对于真实轨道的误差保证。

## 解析与行为验证

- 不等间隔三次多项式：Hermite 3/5/7 同时恢复位置及导数；大绝对历元、零点两侧 1 ns；两者容限 `2e-9`（SI）。
- 倾斜圆轨道参考分别验证多项式算法；另以圆轨道解析解测量三阶/七阶、120/60 秒采样的误差收敛。七阶 60 秒最大位置范数误差须小于 `1e-4 m`，比 120 秒至少缩小 64 倍；同为 120 秒，七阶至少比三阶小 1000 倍。
- 精确节点逐位返回；Lagrange 速度不使用位置差分；包含左右包围节点的最近连续窗口、不均匀节点的局部区间及距离相等时的选择；零阶保持与间断列表。
- 共享端点前后 1 ns、机动后状态优先、缺口和覆盖外拒绝；空表、重复/乱序、重叠、参考系/原点不一致、非有限值、未知枚举及阶数不足拒绝；浮点无法分辨的时间窗口拒绝。
- 输入与选项所有权、创建和查询取消、取消后复用、乱序和并发；Session / Scenario 及隔离安装后的 Python、C++ SDK。
- 用 HPOP 点质量模型在 ±600 秒采样，再用两种七阶插值查询非节点，与同一 HPOP 直接积分比较：每轴 `1e-3 m`、`1e-5 m/s`。此项检查集成接口；独立数值真值来自上面的 Hipparchus 和解析参考，不把同实现对照当成外部验收。

运行日志、JUnit/GoogleTest XML、误差测量和安装产物写入新的 `out/validation/` 目录。禁止通过放宽容限掩盖实现误差。高阶病态数据不保证全局误差，数据分段政策和窗口选择仍需实测 STK；本协议不声称已完整实现 HPOP 科学插值。

本次数值核对的最大逐分量差：位置 `8.722417987883091e-8 m`、速度 `1.3387762010097504e-9 m/s`。七阶 60 秒圆轨道采样的最大位置范数误差分别为 Lagrange `2.284138628502547e-6 m`、Hermite `1.6854510426540375e-8 m`；减半步长与升阶门限均通过。运行日志保留在 `out/validation/hpop-interpolation-20261017/`，这些指标仅对应本协议的固定样例。

## HPOP 连续传播节点

`HpopEphemeris` 与 `HpopEphemerisSession` 验证真实积分节点进入插值的完整路径。固定输出遵循 [STK Integrator](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm) 的公开语义：积分步在报告时刻截短，也可经过多个较小步骤后才输出。当前天气跳变分段、局部窗口选择与 Gauss–Jackson 非完整步重启策略是 OrbitForge 的明确实现，尚无 STK 安装可实测其内部行为。

- 五种积分器分别使用接受端点及 `120.123456789 s` 固定输出，对照 `bulirsch_stoer.txt` 中全部 60 组 Hipparchus 4.0.2 参考，共 600 组终点。包括正反向传播、中心引力、J2 和移动月球。参考输入、生成器与散列沿用[HPOP 验证协议](hpop_protocol.md)，不重新生成或改写固定数据。RK4 步长为 2 s，Gauss–Jackson 为 30 s；自适应方法最大 60 s，相对容限 `1e-14`，位置/速度绝对容限分别 `1e-8 m`、`1e-11 m/s`。位置、速度范数差门限沿用 `5e-3 m`、`5e-6 m/s`。
- CSPICE `prop2b` 是独立轨道参考：倾斜圆轨道和偏心率 0.72 的轨道、五种积分器、正反向 6000 s、Lagrange/Hermite 七阶，以及接受节点/30 s 固定输出，合计 400 次查询。覆盖端点内侧 1 ns、非整秒及结束节点。RK4 步长为 2 s，Gauss–Jackson 为 10 s；自适应方法最大 30 s，容限同上。两项范数门限为 `5e-3 m`、`5e-6 m/s`。
- NRLMSISE-00 文件天气在 2016 年末闰秒后的 UTC 午夜施加 Ap 跳变；五种积分器、正反向、接受节点/37 s 输出均保留共享边界并分段。查询边界及其两侧 1 ns，与 RKV89 最大步长 7 s 的直接积分逐分量比较（位置 `1e-3 m`、速度 `1e-6 m/s`）。这是边界与路由验收；独立大气算法对照见阻力协议，不把同实现比较当成外部真值。
- 精确整数纳秒输出覆盖绝对历元 `±8e18 ns`、正反向非整秒终点、1 ns 输出网格，以及零时长；无法在相对 double 秒中区分的最后一步明确失败。超过 366 天 1 ns 及跨越整个 int64 Epoch 范围均拒绝，避免有符号溢出。
- 只覆盖请求区间的测试月球 Provider 检查五种积分器在 `256 天 + 3 ns` 的正反向端点不越界。已复现长区间浮点取整可能越出精确结束时刻的问题，并验证修复；结束端点保留原始 Epoch，中间转换限制在实际滑行区间。Session 建成后禁用该 Provider，查询仍成功且力求值次数不增加。
- 取消创建不返回部分结果，重试等于新建；输入按值捕获，Python 取消回调中修改原输入不改变传播；已生成 Session 支持乱序/并发查询，越界、无足够节点及非法输出间隔拒绝。独立安装后的 Python 和 C++ SDK 运行相同公开入口。

上述固定样例的最大范数差：600 组 Hipparchus 终点为位置 `0.0027621587520575792 m`、速度 `2.9779592026041214e-6 m/s`；400 次 CSPICE 插值查询为位置 `2.5263617226058819e-5 m`、速度 `1.0520327502330483e-7 m/s`。这些数值不构成任意轨道或任意步长的精度承诺。命令、误差属性、完整回归和独立安装日志保存于 `out/validation/hpop-ephemeris-20261017/`。

## 项目自动装配与机动分段

`HpopProjectInterpolation.*` 在普通 `make_orbit_session` / `Project.prepare()` 路径验证两种七阶插值、全部五种积分器。独立数值真值仍来自 CSPICE `prop2b`，脉冲参考直接按给定 GCRF 三轴 Δv 更新速度，不调用生产机动实现。

- 250 次短弧查询：相对初始历元的区间 `[-45,-30]`、`[-1,1]`、`[30,45]`、`[0,1]`、`[-1,0]` 秒，在端点、端点内侧 1 ns 和中点求值；绝对历元 `8e18 ns`。RK4 / GJ 固定 2 s，其余使用默认自适应配置。位置/速度范数差须小于 `1e-3 m` / `1e-6 m/s`。
- 70 次机动查询：初始脉冲、137.123456789 s 滑行、同历元两次脉冲、8.123456789 s 短滑行及末端脉冲；检查机动前后 1 ns、精确点火状态、四条独立记录、停止前缀和结束后拒绝。位置/速度范数差须小于 `5e-3 m` / `5e-6 m/s`。
- 上述两组实测最大位置/速度范数差分别为 `9.9730092046591679e-8 m` / `8.4433079570627125e-8 m/s`，以及 `1.9489929502716854e-7 m` / `1.8875521222523257e-10 m/s`。指标只适用于这些短弧配置。
- `WeatherFiles.HpopEphemerisSplitsTheLeapSecondWeatherJumpInBothDirections` 另覆盖跳变两侧各 3 s 的短天气段：两种七阶插值、全部五种积分器、正反向、边界 ±1 ns。支持点必须落在各自段内；相对 RKV89 最大 0.2 s 的直接积分保持逐分量 `1 mm` / `1 µm/s` 门槛。这是分段和收敛测试，独立密度对照沿用大气协议。
- 构造只验证环境；首次数值生成可以取消，随后重试成功。测试 Provider 在发布缓存后拒绝求力，乱序与并发查询仍成功；桌面通过带检查点的 `discontinuities` 生成间断列表。
- 普通状态、单点报告与显式固定输出 Session 逐位相同；启用固定输出时修改轨道步长改变分析/报告身份，关闭时不改变；旧分析不可变。当前 v40 / OBJS31 固定样例由 `struct.pack` 独立组装，插值 `<IIdB>` 与正则化 `<BdI>` 字段均有双层 CRC 及逐字节重写检查，精确字节与 SHA-256 见[样例说明](../../tests/integration/fixtures/README.md)。非法方法、阶数、布尔值及所有非当前版本拒绝。
- Qt offscreen 检查全部 HPOP 控件。新增交互覆盖偶数 Hermite 拒绝、切换保留各自草稿、键盘步进、应用、取消、重开和 430/720 逻辑宽度；原生 Windows 实际 DPR 1.5 / 2.25 各生成四张截图，人工核对代表性宽窄布局。独立 Python 3.13 与安装后 SDK 检查同一配置、公开入口和取消重试。

命令、错误诊断、数值属性、独立安装和截图保留于 `out/validation/hpop-project-interpolation-20261017/`。短段支持点、固定输出点数不足时拒绝和局部窗口策略属于 OrbitForge 的公开政策，不能以本项通过代替 STK 实机数值验收。

## VOP 通用变量与独立 μ

固定 JAR 版本仍为 Orekit 13.1.2 / Hipparchus 4.0.2，SHA-256 由 `tools/validation/orekit/dependencies.json` 核验。新生成器不调用 OrbitForge，不增加运行依赖，输出 ASCII/LF；两次生成应逐字节相同。

```text
javac -cp <jars>/* -d <classes> tools/validation/orekit/OrbitForgeUniversalKeplerReference.java tools/validation/orekit/OrbitForgeVopInterpolationReference.java
java -cp <classes>;<jars>/* OrbitForgeUniversalKeplerReference > universal_kepler.txt
java -cp <classes>;<jars>/* OrbitForgeVopInterpolationReference vop_interpolation.txt
```

Linux 使用 `:` 分隔 classpath。通用变量基元对照 Orekit Cartesian NumericalPropagator + Hipparchus DP853 的直接 Newtonian 积分；参考不使用生产 Stumpff 求根器。80 组包含圆轨道、偏心率 .01/.7/.95/.999999/1/1.000001/1.4、倾斜方向、GEO、非零径向速度，以及正反向 1 ns、123.456789/6000/86400 s。参考自身用最大步长 10/5 s 检查，最大分量差 `1.16136e-5 m` / `1.18525e-8 m/s`；生产与参考以及来回映射的每轴门限均为 `5e-4 m` / `5e-7 m/s`。固定文件 `universal_kepler.txt` SHA-256 为 `ccce99eb79a1fa79b5d15d960ecc2eab6cb03d5f3d5f8bbce7b46f656bea8b93`。

VOP 插值生成器使用 Orekit KeplerianPropagator 搬移节点，以 Hipparchus Neville 插值计算共同历元的参数，独立于生产重心算法。这是对 OrbitForge 公开配方的独立核对，不是 STK 导出的真值。153 组 / 1059 查询覆盖全部 0–29 阶、非均匀节点、圆/椭圆/双曲轨道、J2、正确 μ 和偏离底场 1% 的独立 μ、端点及内侧 1 ns。Core 和安装 Python 使用相同固定输入；绝对历元加上 `8000000000000000123 ns`。位置/速度范数差门限为 `2e-4 m` / `2e-7 m/s`，并分别验证纯二体真值。固定文件 `vop_interpolation.txt` SHA-256 为 `ed1d3d0ebcaf1869c25114e742ce64fc5d86deffd03505edef3e7750044de1f5`。

另用 Orekit `J2OnlyPerturbation` 与 DP853 直接轨迹验证实际插值误差：GM `3.986004415e14 m³/s²`、参考半径 `6378136.3 m`、J2 `1.0826266835531513e-3`、固定 GCRF 对称轴、初态 `[7e6,0,0;0,6800,3000]` SI，七阶、17 个节点，间隔 600/300/150 s。参考自身最大步长 10/5 s 差为 `1.86265e-9 m` / `1.81899e-12 m/s`。两次减半均要求位置和速度误差至少减小 64 倍；150 s 的门限为 `2e-3 m` / `2e-6 m/s`。不预设 VOP 对任意轨道或普通 Lagrange 的固定改善倍数。

固定样例实测：通用变量基元最大分量差 `1.11890e-5 m` / `1.19699e-8 m/s`，来回位置差 `3.69223e-6 m`；VOP 插值最大范数差 `1.55328e-6 m` / `2.76762e-9 m/s`。J2 600/300/150 s 的位置范数误差分别约 `79.5952 m`、`0.316210 m`、`0.00101074 m`，速度误差约 `0.0670958`、`0.000320939`、`1.07987e-6 m/s`。同一 300 s 样例普通 Lagrange 的位置误差为 `1.20329 m`，仅作为此配置的比较数据。这些是固定模型下的数值误差，不代表观测轨道预报精度。

行为与集成检查包括：

- 地心 GCRF / SPICE J2000，拒绝非惯性轴、零半径、径向轨道、非法 μ；精确整数节点、零阶二体推进、右连续机动、缺口及覆盖外拒绝、取消重试与并发。
- VOP 扩展到全部五种积分器、普通/正则化时间、正反向短弧、机动、最高阶及纳秒短段，独立 CSPICE `prop2b` 门限沿用原协议。天气跳变、阴影接触段两侧及取消路径也覆盖三种插值；不跨分段补点。
- 活动 μ 改变分析、报告及 Session 身份，非活动 μ 保存但不改变这些身份；旧分析保持不变。v40 / OBJS31 固定样例由 `struct.pack` 独立编码 μ，跨 C++ / Python 逐字节重写；CRC 修复后的非法字段及全部非当前版本仍拒绝。
- Qt offscreen 和原生 Windows 验证独立 μ 的单位切换、灰显草稿、非法输入、方法各自阶数、确认/取消/重开；430/720 逻辑宽度、实际 DPR 1.5 / 2.25，人工核对窄宽截图。独立安装 Python 3.13、C++ SDK 消费端和九个示例均通过。

完整日志、初次接入问题及修正后的结果保存在 `out/validation/hpop-vop-20261017/`。VOP **插值**与独立 μ 由本协议验证；VOP 积分使用[独立对照协议](vop_integration_protocol.md)，协方差插值仍待实现。
