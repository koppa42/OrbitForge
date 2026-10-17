# 太阳辐射压

HPOP 的球形太阳辐射压由 Core 实现，桌面、C++、Python、报告与保存后的重新分析使用相同计算。它是 [HPOP 对齐清单](hpop_alignment.md)中的一个增量：已支持球形卫星、独立 Cr 与面积质量比、距离平方缩放、无阴影／圆柱／双锥、地球／月球、七颗行星及冥王星的球面遮挡配置及地球大气等效高度。

## 输入与计算

所有计算参数使用 SI。太阳方向在每个积分阶段按所选方法求值；遮挡体中心使用接收历元的地心几何位置。令 `s` 为卫星指向所选太阳位置的向量，计算

```text
a_srp = −lighting_ratio · P_ref · (d_ref / |s|)² · Cr · (A_srp / m) · s / |s|
```

默认 `P_ref = 4.56e-6 Pa`、`d_ref = 149597870700 m`、`Cr = 1`、`A_srp/m = 0.02 m²/kg`；该面积质量比独立于阻力。太阳、地球、月球球面半径分别为 `695700000`、`6378137`、`1737400 m`。全部常数可显式修改并参与有效输入身份；不将这些默认值当成所有 STK 配置的默认值。[Orekit 13.1.2 光压实现](https://github.com/CS-SI/Orekit/blob/13.1.2/src/main/java/org/orekit/forces/radiation/SolarRadiationPressure.java)

Cr 范围为 `[-100, 100]`，面积质量比为 `[0, 9999] m²/kg`，允许带符号的经验 Cr 与零面积。光压、参考距离和球面半径必须有限且为正，大气高度必须有限且非负，组合半径必须有限。所有设置草稿都须是有效数值；禁用时仍保存有效草稿。启用时卫星位于太阳内部或已分配遮挡球体内部会明确失败。[STK HPOPForce 参数范围](https://help.agi.com/stkEngineOnUNIX/Subsystems/connectCmds/Content/cmd_HPOPForce.htm)

`SrpShadowModel::none` 使用全照；`cylindrical` 按太阳方向射线是否穿过任一分配球体判定 0/1；`dual_cone` 使用均匀亮度的视太阳圆盘，支持全照、半影、本影和环食。双锥在太阳方向的局部角平面上求所有已分配天体视圆盘并集与太阳圆盘的交面积，以 Green 定理分段圆弧积分求遮挡比例。重叠遮挡只计一次，不能把两个独立照明比例相乘。地球等效半径为球面半径加大气高度；月球不加该高度。

当前轮廓为球面；地球椭球轮廓、遮挡体自身的视位置修正、其他卫星及小天体目录仍待实现。边界修正已提供独立开关，默认关闭；关闭时仍按各积分阶段求值。[STK SRP 设置](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)

## 太阳位置方法

`SrpSunPositionMethod` / `SrpSettings.sun_position_method` 保存三种独立方法，与 [STK HPOP 的太阳位置选项](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)对应：

| C++ / Python | 计算 |
| --- | --- |
| `geometric` / `"true"` | 接收历元的几何太阳，减卫星位置，无光行时或光行差 |
| `apparent_to_true_cb` / `"apparent_to_true_cb"`（默认） | 以地心为观测者求接收型 LT+S 视太阳，再相对卫星构造向量 |
| `apparent` / `"apparent"` | 以当前积分阶段的卫星位置和速度为观测者，求接收型 LT+S 太阳向量 |

OrbitForge 默认采用地心视太阳；几何太阳的独立 Orekit 参考显式选择 `true`。公开帮助定义功能含义，并不完整披露 HPOP 内部星历、光行差或遮挡体历元约定；这三种实现不等于已通过 STK 同配置数值验收。

卫星方法采用 [NAIF `spkapo` 的单次 LT+S 约定](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/spkapo_c.html)：先由几何距离估计光行时，查询太阳发光时刻，再按观测者质心速度计算光行差。实现将各步显式展开，查询前校验发光时刻；不允许内部查询越过快照边界。速度包含地球的 SSB 速度及卫星相对地球的速度，后者从 TAI 导数换到 TDB 导数。换算使用当前 LSK 的 `K、EB、M0、M1`，按 [NAIF TIME 的公式](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/time.html)求 `dTAI/dTDB = 1 − K cos(E) M1 (1 + EB cos(M))`。这是 ICRF 对齐轴下的牛顿叠加，不是完整 BCRS/GCRS 相对论转换。

第三体引力仍独立使用几何位置；切换光压太阳方法不改变其星历算法。地月遮挡中心目前仍为几何中心，已在界面说明与算法来源中记录；不能将此等同于 AGI Components 对所有辐射向量的视位置修正。

## 资源、来源与持久化

光压启用时总是需要太阳星历，启用阴影且分配月球时还需要月球。第三体引力勾选状态与此独立，GM 不参与光压力值。每个积分阶段使用对应历元的 Provider；准备阶段检查完整传播区间与实际目标链。缺文件或覆盖不足会停止准备，不能自动关闭光压。太阳向量由现有 ICRF 对齐的 SPICE 力模型适配器提供；SDK 不隐式下载资源。地心视太阳认证起点前 600 秒的历史。卫星视太阳不假定固定地心距离：准备时验证整个接收区间的 Earth→EMB→SSB 链，以及从快照起点到接收区间终点的完整 Sun→SSB 链；每次求值另检查实际发光时刻。过去的太阳链有洞即拒绝准备，不能以只查接收端点替代；若实际距离所需历史早于快照起点，求值失败，不外推。

会话和分析持有不可变星历；来源包含 `spherical-uniform-angular-union-v3`、太阳方法、`NONE` / `earth-LT+S-v1` / `spacecraft-LT+S-tai-rate-v1` 及 `occultors=geometric`、有效光压参数及星历解释／内容身份。禁用光压的草稿，以及无阴影或未分配遮挡体的半径草稿不改变分析和报告身份。项目 v40／OBJS31 保存全部有效草稿；不提供旧版本解码。

```python
data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", manifest, workspace)
orbit = of.orbits.Hpop(
    initial=initial, gravity_degree=0,
    srp=of.orbits.SrpSettings(enabled=True, cr=1.2,
                             area_mass_ratio_m2_per_kg=0.02,
                             shadow_model="dual_cone", sun_position_method="apparent_to_true_cb",
                             earth_shadow=True, moon_shadow=True))
sat = project.objects.satellite("SRP", orbit=orbit)
analysis = project.prepare(data=data)
trajectory = analysis.states(sat, step=of.units.s(60))
```

C++ 使用 `HpopOptions::srp`／`SrpOptions`，通过显式 `HpopEnvironment` 或 `Project::prepare(data)` 装配资源。低层 `evaluate_srp` 返回加速度与照明比例，便于独立几何验证；它消费调用者已经选好的太阳向量，不自行解释太阳方法或查询 Provider。Python 提供对应原生 `SrpOptions`、`SrpSunPositionMethod`、`SrpShadowModel`、`SrpEvaluation`、`evaluate_srp`，易用接口使用不可变 `of.orbits.SrpSettings`。

独立 Orekit 轨迹参考、独立面积积分和边界测试见 [HPOP 验证协议](../validation/hpop_protocol.md)。这些验证不代表同配置 STK 的直接轨迹对照或实际预报精度。

## 共享卫星质量

`HpopOptions.satellite_mass_kg` / `HpopSettings.satellite_mass_kg` 默认为 1000 kg，必须有限且正。“其他”页支持 kg、g、t；阻力和光压页的面积质量比仍是独立输入，并显示只读的对应面积。C++ `drag_area_m2()` / `srp_area_m2()`（Python 同名只读属性）返回各自 A/m 与质量的乘积，拒绝溢出或非零乘积下溢为零；零 A/m 允许零面积。

修改质量只改变对应面积，球形加速度、轨迹与计算身份保持不变；设置仍写入项目并标记为已修改。未来非球形或插件模型使用质量时，必须把实际使用的质量纳入身份。此行为依据 [STK Satellite mass](https://help.agi.com/stk/Content/hpop/hpopAdditional.htm)，独立面积/质量模式不作为球形模型的第二套输入。

## 可用／已分配遮挡天体

桌面 SRP 阴影组的“选择遮挡天体”打开可搜索的 Available / Assigned 双列表，支持增加、移除、多选和双击。地球、月球仍使用各自半径；额外行星使用独立可编辑的球面半径。选择窗口取消只撤销其修改，主 HPOP 窗口取消撤销整个草稿；宽窄布局保持输入及单位。[STK 官方列表语义](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)

`SrpOptions::additional_eclipsing_bodies` / `SrpSettings.additional_eclipsing_bodies` 支持水星、金星、火星、木星、土星、天王星、海王星和冥王星，不能与 Earth/Moon 字段重复。Python 例如 `SrpSettings(additional_eclipsing_bodies=(SrpEclipsingBody("venus"),))`。起始半径取 [NAIF pck00011](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/pck00011.tpc) 的最大轴（IAU 2015）：2440530、6051800、3396190、71492000、60268000、25559000、24764000、1188300 m。它们是显式可修改的球形近似，不代表 STK `.cb` 形状数据或椭球轮廓。

行星遮挡使用 199、299、499、599、699、799、899、999 **本体中心**，不使用第三体引力采用的系统质心 4–9。Core 的 `IThirdBodyEphemeris` 新增可选 `eclipsing_body_position` 与完整区间认证；默认只复用水星、金星已有本体位置，其余未实现的 Provider 明确拒绝。SPICE 连续覆盖核验检查本体→系统质心→SSB 和 Earth→EMB→SSB 的全部链与内部缺口。DE440s 可直接提供 Earth/Moon/Mercury/Venus。火星、外行星及冥王星可在数据中心导入本体 SPK；导入器只保留本体相对系统质心的片段，按传播区间认证覆盖，详见[补充星历契约](spice.md#行星本体补充星历)。未导入或覆盖不足时明确失败；更多卫星和小天体仍待接入。

低层 `evaluate_srp` 可传 `SrpBodyPosition` 列表，与每个已分配额外天体一一匹配。缺失、重复、未分配或非有限位置拒绝；太阳、卫星与遮挡体在同一接收历元和同一惯性轴，调用方负责所选太阳方法。最多十个遮挡圆盘同时求并集，重合或包含只计一次。计算按固定天体顺序求和，因此列表重排不改变力、轨迹或计算身份。圆柱模型即使已判全影仍检查其余几何，避免掩盖卫星位于另一遮挡球体内部的错误。

175 组多圆并集与独立 SciPy 1.16.3 QUADPACK 垂直切片积分对照；4 条包含点质量地球、三个合成运动行星与 SRP 的 LEO/GEO 正反向轨迹，与同版本 DOP853 对照。它们验证多体叠加与路由，不是实际行星半径或 STK 实测轨迹。协议见 [HPOP 验证](../validation/hpop_protocol.md)。

## 阴影边界修正

`SrpOptions::boundary_mitigation` / Python `SrpSettings.boundary_mitigation` 对应 STK 的 Use Boundary Mitigation，默认 false。STK 公开帮助说明它用于修正跨阴影边界的积分误差，没有公开内部算法。OrbitForge 的实现采用可复核的事件定位、单侧力段和高精度边界校正；不声称与 STK 内部代码相同，也尚无 STK 同配置实测数据。

- 使用实际选定的太阳位置方法与几何遮挡体中心。每个球面的外接触、本影内接触和环食内接触采用有符号角距；圆柱采用点太阳接触。接触的前后方与深度门限、天体排序和有效性检查与瞬时光压力一致。
- 在已接受试步内按至多 60 s 的检查间隔检测变号，并对包围的局部极值搜索短暂掠影。求根使用积分前缀；唯一的一阶位置外推是距段首四个时间分辨单位的选侧探针。时间分辨率为 `max(1 ns, 16 ε max(|t0|,|t1|))`，相邻不可分辨事件按同一边界处理。有限检查间隔不能保证捕获任意快速振荡的自定义星历或所有相切退化情况，不作为通用食事件搜索 API。
- 圆柱光照比例在每个力段内保持单侧取值，求出边界后再切换。双锥圆弓形面积在接触处具有 3/2 次幂起始行为；半影及接触相邻步使用误差受控的 Bulirsch–Stoer 前缀校正（相对尺度 1e−15，位置/速度绝对尺度 1e−10 m / 1e−13 m/s）。全部物理力在该前缀一起积分，不平滑或削弱光压力。平滑日照/本影段继续使用所选方法。
- 五种积分器、固定/相对误差控制、正反向传播和 Sundman 时间正则化共用事件语义。固定物理或 q 网格不因接触重新锚定；GJ 在接触或非光滑半影桥接后重新建立差分历史。正则化接触前缀积分等价的 `(r,v,q)` 方程，不投影时钟或伪造状态。真实积分工作继续计入每次查询 200000 次尝试上限，检测和求根支持取消。
- 科学星历以接触时刻分弧，不能跨边界构造插值模板。浮点事件状态先按动力学输运到其整数纳秒历元，避免 Hermite 放大节点时间标注误差。接受节点模式遇到短半影弧时，在该弧内增加真实积分停止点并重新定位事件，在边界两侧的插值窗口以高精度 BS 前缀核对中点，误差超过配置分量尺度的十分之一时再细化；至多八次并共享每次查询的工作上限。无法满足所选阶数或精度时明确报错。固定报告网格继续保留原配置，短弧节点不足时报错，不静默降低插值阶数。
- 界面位于 Force Models / SRP / 阴影；禁用条件保留草稿。关闭 SRP、None 阴影、无分配遮挡体、Cr 或 A/m 为零时，开关不参与有效计算身份。生效时来源包含 `srp-boundary=contacts-one-sided-bs-v1`、检查间隔和时间分辨率。项目使用 v40 / OBJS31。

固定参考 `tests/reference/propagators/srp_boundaries.txt` 由 `tools/validation/orekit/OrbitForgeSrpBoundaryReference.java` 生成，依赖同目录 `OrbitForgeSrpReference.java` 的合成移动太阳：Orekit 13.1.2、Hipparchus 4.0.2 DP853、TAI/GCRF、球形地球，Cr=1.7、A/m=20 m²/kg，显式放大光压以检验边界误差。LEO/GEO 各含正反向和圆柱/双锥，共 8 条轨迹及所有接触状态。圆柱参考使用 Orekit `EclipseDetector` 的零半径发光源，保留 STK 指定的卫星至太阳方向；不能直接替换为使用地心太阳方向的 `CylindricalShadowEclipseDetector`。

参考自身将最大步长从 2 s 减至 1 s，最大状态分量变化 1.930e−5 m / 8.284e−9 m/s，事件变化 1.776e−9 s。OrbitForge 对五种积分器、普通/正则化时间、两档步长比较 160 条传播；高阶方法的固定验收限为 1 mm、1 µm/s、0.3 µs，RK4 为 0.2 m、0.2 mm/s、30 µs。RK4 减半后误差须至少缩小八倍（1e−4 m 参考/浮点底限）；其他方法要求减半不出现超过固定底限的恶化。当前最大差异：高阶方法 2.556e−5 m / 1.435e−8 m/s / 2.322e−9 s；RK4 粗步长 0.041574 m / 2.331e−5 m/s / 3.513e−6 s。另有 24 条 RKF/RKV/BS 显式固定步长、普通/正则化时间的独立轨迹对照。既有太阳/多体并集、天气跳变和时间正则化参考继续独立保留。

## 变分计算基础

C++ / SDK 提供 `srp_differential`，返回固定所选太阳和几何遮挡体时的状态块、太阳位置块、照明梯度及 `K=Cr×A/m` 偏导。支持现有十体球面并集，并有 Orekit、独立高精度圆盘面积和 SciPy 对照。接口范围及卫星视太阳链、事件处理、协方差尚待接入的边界见[SRP 局部偏导](srp_differential.md)。
