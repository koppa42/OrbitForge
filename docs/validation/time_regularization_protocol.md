# HPOP 时间正则化验证

本协议验证 Sundman 方程、共用积分内核、有界物理时间端点及公开 HPOP 管线。C++ / Python / 项目保存 / Integrator 页共用配置，覆盖整数 TAI、真实天气分段、任务脉冲和科学插值；独立参考来自 Hipparchus 与 CSPICE。本项不替代尚未进行的 STK 同配置实测。方程、尺度、非椭圆限制与控制政策见[计算契约](../architecture/time_regularization.md)。

## 独立参考

`tools/validation/orekit/OrbitForgeSundmanReference.java` 直接调用固定 **Hipparchus 4.0.2** 的 `IterativeLegendreGaussIntegrator`、`DfpField(32)` 和 `DormandPrince853FieldIntegrator`，不调用 OrbitForge。依赖下载地址与 SHA-256 在 `tools/validation/orekit/dependencies.json`；运行前逐个核验 JAR。

`tests/reference/propagators/sundman.txt` 的 ASCII/LF SHA-256 为 `8817fd254063bc4a8c635358b580f31e616b15832e6ac378ad34de8e0993478a`。普通测试只读取固定文件，不联网或重新生成。文件包含：

- 36 行 `P e n Q0`：e = 0、0.7、0.98、0.9999；n = 0.1、0.5、1、1.5、2、2.5、3、5、10。独立十六点 Legendre–Gauss 在偏近点角上求积，绝对/相对容差均 1e−14；生产使用 Simpson，n>2 时改用真近点角，避免两端复用同一求积实现。
- 54 行 `T kind e n q_end r0[3] v0[3] r[3] v[3] t`：两种力 × e=0/0.7/0.98 × n=1/1.5/2 × −0.375/0.4375/1.125 圈。输出时刻由七维方程积分得到，单位为真实秒；正则化坐标 q 无量纲。

μ = 3.986004415e14 m³/s²，近地点 7000000 m，半长轴 a=7000000/(1−e) m；初态在轨道面内转 0.4 rad，再绕 x 轴倾斜 0.6 rad。使用固定惯性直角坐标与连续相对秒，刻意隔离真实历表、UTC 和地球旋转。kind=0 为点质量；kind=1 加上 J2（J2=√5×0.484165371736e−3，R=6378136.3 m）以及各轴 `1e−7 sin(t/1900+0.7i) − 1e−12 v_i` m/s²。后者要求力使用物理时间与速度，不能把 q 或 dr/dq 代入。它是受控数值问题，不代表实际环境模型。

参考将长度按 a、时间按 T0=√(a³/μ) 无量纲化，用 32 位十进制 Dfp 运算，DP853 绝对容差 1e−20、相对容差 1e−18、最小步长 1e−16。每条正则化轨迹以 Q0/256 与 Q0/512 上限重算，保存细步结果；再以独立的普通时间方程传播到该真实秒，时间步上限为 2π/2048。生成器仅在两类检查均通过后写文件：

| 参考质量检查 | 位置范数差 m | 速度范数差 m/s | 时钟差 s |
| --- | --- | --- | --- |
| q 上限减半的实际最大值 | 3.14136e−8 | 1.84041e−12 | 2.03455e−9 |
| q 上限减半的拒绝界限 | 1e−6 | 1e−9 | 5e−9 |
| 普通时间方程交叉比较的实际最大值 | 1.57213e−5 | 9.53076e−9 | 不适用：指定相同时刻 |
| 普通时间方程交叉比较的拒绝界限 | 5e−5 | 5e−8 | 不适用 |

两种参考检查界限均不超过生产绝对误差预算的 1%。生产从圆整后的笛卡尔初态重建 a0/e0，参考归一化采用生成初态的 a/e；高偏心率下的初始能量舍入差也计入生产比较，未从误差中扣除。

## 生产内核验收

`All/SundmanMethods.FixedRegularizedStepsMatchIndependentHighPrecisionAndConverge` 使用与普通 HPOP 共用的 RK4、RKF7(8)、RKV8(9)、BS 和 GJ 内核。GJ 分别执行 Full / Pseudo，共六种设置，每种 54 行、粗细两组，合计 **648 次完整终点比较**。每条轨迹按 q_end 均匀划分，细组精确加倍粗组步数；检查 GJ 每条轨迹实际完成超过 100 个多步推进，避免只验到起步器。

| 方法 | 每圈粗步数；细步数加倍 | 最大位置范数差 m | 最大速度范数差 m/s | 最大时钟差 s |
| --- | --- | --- | --- | --- |
| RK4 | 262144 | 2.77991e−5 | 5.06138e−10 | 2.22353e−8 |
| RKF 7(8) | 4096 | 2.38487e−5 | 5.09532e−10 | 2.24682e−8 |
| RKV 8(9) | 4096 | 2.93436e−5 | 6.96225e−10 | 4.07454e−8 |
| BS 固定十八阶 | 2048 | 2.66512e−5 | 5.20493e−10 | 2.35159e−8 |
| GJ Full | 0.00097228 | 5.89426e-07 | 0.000495529 | 3.00983e-07 |
| GJ Pseudo | 0.000960898 | 5.82525e-07 | 0.000500119 | 3.0377e-07 |

所有粗细结果均须满足 **5 mm、5e−6 m/s、5e−7 s**；同条轨迹的步数加倍变化分别小于 **1 mm、1e−6 m/s、2.5e−7 s**。六种设置的步数加倍最大变化为 4.32283e−5 m、7.24049e−10 m/s、1.29687e−7 s。这些界限验证方程变换与积分一致性，不代表未来 TAI 端点拟合已达到纳秒精度，也不代表实轨预报精度。

高偏心率 n=2 的远地点附近，每个 q 步可跨越较长物理时间；时变摄动力额外要求足够取样。固定步 GJ 的校正收敛不保证截断误差合格，粗步可能失稳；无限增加步数又会进入时钟舍入平台。上述步数由保持原门槛的步数加倍检查确定，并非建议所有用户场景采用该默认值。普通时间 GJ 保持原有 64ε 收敛标准；七维混合方程的内核试验使用位置、dr/dq 范数及真实时间尺度的 8ε。起步器使用 BS 至少十四阶、相对容差 1e−14，位置/dr/dq/时钟绝对容差分别 1e−9/1e−8/1e−11。

另有独立解析和边界检查：

- n=1 / 2 分别对照偏近点角 / 真近点角的解析位置、速度和开普勒时间，三种偏心率、三种有符号圈数、BS/GJ 共 36 个终点，使用相同绝对门槛。
- 36 个每圈归一化值：相对界限 2e−12；e=0.9999 使用 2e−10，包含由圆整笛卡尔初态重建偏心率的条件数放大。此极限仅验证归一化，不扩大轨迹验收范围。
- 坐标往返、传入力的真实时间与速度；Pseudo 保留物理摄动并更新全部变换因子；非有限输入、无周期轨道、径向轨道和非法指数；中点子步取消不得发布部分状态。
- 克隆相同 GJ 历史后比较正反向单步：两种校正的状态相同，Pseudo 恰好少一次收敛后的完整力调用。分别长期传播后的历史与迭代次数可不同，不能从总次数差推导逐步上限。

## 指定物理时间端点验收

`All/SundmanBoundaries.PhysicalEndpointsMatchIndependentReferenceWithoutOutOfRangeForces` 将同一独立文件中的真实秒作为终点，对五种算法的固定模式（GJ Full/Pseudo 分开）和 RKF/RKV/BS 自适应模式进行 **9×54×2 = 972 次最终状态比较**。每条轨迹经过 t/3、2t/3、t 三个顺序查询端点，保留状态和 q 网格；每次查询单独检查 200000 次尝试预算。力回调断言每个实际调用均在已声明的物理时间区间内。

固定粗步每圈 RK4 为 131072，其余方法点质量为 4096、时变摄动力为 32768，细步加倍；自适应同时把物理上限从 300 s 减至 150 s，相对容差 1e−14、位置/速度绝对容差 1e−8 m / 1e−11 m/s。边界逆方程保留同一方法；GJ 使用 BS 十四至十八阶起步器，局部 r/u/t 绝对尺度为 1e−9 m / (1e−12 m/s × g) / 1e−11 s。

| 方法 | 最大位置范数差 m | 最大速度范数差 m/s | 步长减半位置变化 m | 步长减半速度变化 m/s |
| --- | --- | --- | --- | --- |
| RK4 固定 | 0.000563998 | 1.02373e-08 | 0.000535238 | 7.39004e-09 |
| RKF 固定 | 3.82328e-05 | 1.13292e-08 | 1.59986e-05 | 6.92137e-09 |
| RKV 固定 | 9.95785e-05 | 6.03675e-08 | 7.10683e-05 | 4.02394e-08 |
| BS 固定 | 8.05966e-05 | 4.88595e-08 | 6.0507e-05 | 3.66803e-08 |
| GJ Full | 0.00097228 | 5.89426e-07 | 0.000495529 | 3.00983e-07 |
| GJ Pseudo | 0.000960898 | 5.82525e-07 | 0.000500119 | 3.0377e-07 |
| RKF 自适应 | 2.69267e-05 | 8.15546e-09 | 1.76073e-05 | 2.30695e-09 |
| RKV 自适应 | 0.000123081 | 7.46152e-08 | 0.000130911 | 7.93618e-08 |
| BS 自适应 | 8.80643e-05 | 2.34351e-08 | 8.01535e-05 | 2.11466e-08 |

每条粗细轨迹均满足原 **5 mm / 5e−6 m/s** 绝对门槛和 **1 mm / 1e−6 m/s** 收敛门槛。终点秒与输入双精度值精确相等；大历元下整数纳秒映射另由下述公开入口验收。两种 GJ 每条轨迹均完成超过 100 个真实多步推进，各自累计 1924367 步，避免只检验起步器。

`SundmanBounded` 另验全部五种方法的 ±1 ns 端点、取消不修改输入历史、预算耗尽前不调用力，以及拒绝直接反转旧历史。分段常加速度的正反向解析案例，在 ±315.123456789 s 重置力段后继续到 ±700.987654321 s，检查单侧力覆盖、精确终点、严格单调输出与 1e−5 m / 1e−8 m/s 误差。它是事件边界的受控测试，不冒充真实天气数据或机动管线验收。

`GaussJacksonKernel.FirstOrderClockRetainsSubUlpStarterResidual` 用常导数的一次状态解析解验证起步到多步的补偿传递：基值 1e6，初始偏移四分之一 ulp，每步增量半个 ulp，正反向各检查 128 次推进。它可独立复现丢弃起步残差导致的舍入偏差。

`AdaptivePhysicalLimitsTakePrecedenceOverTheNominalRegularizedScale` 用独立 Kepler 方程与圆锥曲线几何验收物理步长政策：三种自适应方法 × e=0/.7 × 正反向 × 20/20 s 与 20/35 s 两组边界，共 24 例。名义每圈步数分别为 100000000 和 120，覆盖名义 q 对应时间小于 Min Step 与大于 Max Step；在连续段内验证真实接受间隔，末端截短保留精确终点，位置/速度门槛为 1e−5 m / 1e−8 m/s。

`AdaptiveMinimumStepRejectsUnresolvedOscillatoryForce` 为三种算法加入 `sin(100t)` m/s² 摄动，分别用 20/20 s 和 20/35 s 约束；六例均须在 100 次尝试内诊断无法满足最小步长，不得等到 200000 次预算耗尽。输入状态保持不变。上述显式优先级和切换规则属于本项目实现，不能据此宣称与 STK 内部控制器完全相同。

## 普通 HPOP 回归与身份

为降低高阶外推的舍入放大，固定十八阶 BS 和 GJ 十四至十八阶起步器改用 2/4/6/8/12/16/24/32/48 子步，外推局部状态增量。普通自适应 BS 的 2/4/…/18 序列及其控制器保留。对应算法来源升级至 `bulirsch-stoer-midpoint-richardson-fixed18-v2`、`gauss-jackson12-summed-bs14to18-startup-full-v3` 和 `gauss-jackson12-summed-bs14to18-startup-pseudo-v3`；GJ v3 还保留起步和求和重建的舍入补偿。当前项目为 v36 / OBJS27。

普通时间的固定步长、Hipparchus/Orekit 轨迹、天气、缓存、科学插值、任务和 API 测试保持原门槛；GJ v3 的普通时间数值见 [HPOP 协议](hpop_protocol.md#gj-起步与求和补偿-v3)。

## 公开入口、科学插值与整数历元

公开状态查询使用同一份不可变 Hipparchus 文件的 27 条点质量轨迹，覆盖 9 种方法/模式（五种方法、GJ 两种校正及三种自适应模式），共 **243** 个终点。初始历元为 int64 最大值减 40 天；RK4 每圈 131072 步，其余 4096 步，指数 1/1.5/2、偏心率 0/.7/.98、正反向与超过一圈。相对容差 1e−14、位置/速度绝对容差 1e−8 m / 1e−11 m/s，最大物理步长 150 s。最大位置差 **0.000970826496 m**，最大速度差 **5.88544664e−7 m/s**，原门槛仍为 5 mm / 5e−6 m/s。

项目和科学星历以独立 CSPICE 二体解对照；以下最大值包含普通与正则化两条路径，均未放宽原门槛。

| 路径 | 覆盖 | 最大位置差 m | 最大速度差 m/s | 门槛 |
| --- | --- | --- | --- | --- |
| 科学星历 | 两种插值、五种积分器、e=0/.72、±6000 s、接受节点与固定报告 | 4.35430874e−6 | 2.51573367e−8 | 5 mm / 5e−6 m/s |
| 短滑行与大历元 | 五种积分器、两种插值、初始 TAI 8e18 ns、初态前后/跨初态/1 s 段 | 2.88710308e−8 | 2.27418010e−7 | 1 mm / 1e−6 m/s |
| 任务脉冲 | 初始、同历元连续及最终脉冲；脉冲两侧 ±1 ns；后续计划不得越过项目结束 | 4.20547851e−9 | 2.20739913e−11 | 5 mm / 5e−6 m/s |

另验 1 ns 滑行、Lagrange 0/29、Hermite 3/29、精确报告网格、256 天端点、有限力数据范围、取消/重试及生成后不再调用力。真实 NRLMSISE-00/CelesTrak 三小时跳变跨 2016 闰秒，检查两侧共享边界、正反向和每侧仅 3 s 的科学插值；与普通时间 RKV 小步长结果维持 1 mm / 1e−6 m/s。

回归覆盖两个具体失效：浮点秒节点直接标记为圆整纳秒会产生微米级坐标抖动，Hermite 求导将其放大；输出使用连续力段导数作不足数纳秒的时间输运。物理输出门限的短逆方程前缀不得覆盖下一次自适应 q 提议，避免形成不必要的密集节点；完整步依然检查误差和物理 Min/Max。原问题可分别使速度差达到 5.58e−6 与 2.39e−6 m/s；现有独立对照在原容限下通过，参考字节保持不变。

v36 / OBJS27 固定项目样例由独立 `struct.pack("<BdI", 1, 1.25, 240)` 插入三个字段并重算 CRC，1484 字节、SHA-256 为 `288fa30c1acd71d1f7c1202d3e015b6ecd6a2219c0eb97659aa8473138499dab`。C++ 与 Python 精确重写；所有旧/未知版本、非布尔开关、非法指数和越界步数拒绝且保留源文件。关闭后的合法草稿、启用后的普通固定秒步长不参与分析/报告身份；活动指数和步数参与，既有分析保持原状态。

Windows/MSVC Release 完整构建通过：Core **136/136**，集成 **179 通过 / 2 项完整资源条件跳过**，独立 Python 3.13 **221 通过 / 5 项可选 NumPy/pandas 跳过**，安装后 SDK **2/2**。Qt HPOP offscreen **33/33**；原生 Windows DPR **1.5/2.25**、430/720 逻辑宽度检查参数联动、键盘、取消、重开和动态摘要，未出现横向溢出。完整 EGM2008/FES 条件跳过不计为本轮全资源验证。命令、XML、原生截图、失败诊断与最终汇总保存在 `out/validation/hpop-public-regularization-20261017/`，不提交运行产物。


## 复现

Java 21 示例；`$jars` 指向已按 `dependencies.json` 核验的目录。新参考生成较慢，普通 CI 只消费仓库内固定输入。

```powershell
$jars = 'out/validation/orekit/jars'
$classpath = (Get-ChildItem -LiteralPath $jars -Filter '*.jar').FullName -join [IO.Path]::PathSeparator
New-Item -ItemType Directory -Force -Path out/validation/sundman/classes
javac -cp $classpath -d out/validation/sundman/classes tools/validation/orekit/OrbitForgeSundmanReference.java
java -Xmx768m -cp ("out/validation/sundman/classes" + [IO.Path]::PathSeparator + $classpath) OrbitForgeSundmanReference out/validation/sundman/sundman.txt
Get-FileHash -Algorithm SHA256 -LiteralPath out/validation/sundman/sundman.txt
cmake --build --preset desktop-graphics
ctest --preset desktop-graphics -R 'core\.(Sundman|HpopRegularization|GaussJacksonKernel|All/SundmanMethods|All/SundmanBoundaries)'
ctest --preset desktop-graphics -R 'core\.(Hpop|Mission)'
```

平台浮点运算可能有末位差异；比较完整状态及容限，固定参考文件不得为使测试通过而改写。完整集成、独立安装 Python 与 SDK 的命令遵循[构建说明](../development/build.md)和[CI 规范](../development/ci.md)。
