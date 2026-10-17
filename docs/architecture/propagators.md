# 轨道预报器

桌面卫星属性、C++ Runtime 与 Python 共用同一计算实现。预报器在一轮计算中固定输入，支持正向、反向和任意顺序查询。所有状态使用米、米/秒和 TAI `Epoch`，参考系随状态明确返回。

| 模型 | 输入 | 原生输出 | 适用范围 |
| --- | --- | --- | --- |
| Two Body | GCRF 瞬时位置/速度或经典根数 | GCRF | 理想二体轨道、基础设计 |
| J2 secular | 平均椭圆，含平近点角 M | GCRF | 一阶长期趋势 |
| J4 secular | 平均椭圆，含平近点角 M | GCRF | 含 J2、J2²、J4 的长期趋势 |
| SGP4 | 一组固定的 TLE，两行各 69 个 ASCII 字符 | TEME of date | 与 TLE 配套的近地/深空预报 |
| HPOP（开发中） | GCRF 瞬时位置/速度或经典根数；力模型与积分设置 | GCRF | 静态球谐、可选太阳系第三体引力与球形太阳辐射压的数值传播 |

此分工参考 [STK 预报器说明](https://help.agi.com/stk/12.7.1/Content/stk/vehSat_orbitProp_choose.htm)。新增模型不改变已有二体/J2 的模型常数或算法。

机动轨道可选[任务序列](missions.md)：从初始状态按顺序执行滑行和脉冲，滑行使用二体或 HPOP。任务向前定义，区间内支持任意顺序查询；点火时刻返回机动后状态。当前不包含有限推力和自动目标求解。

## HPOP

当前支持静态重力、太阳系第三体与[球形太阳辐射压](solar_radiation_pressure.md)数值传播。通用地球轨道所需的完整力模型、积分高级能力与协方差目标见 [HPOP 与 STK 的对照及重构要求](hpop_alignment.md)；其中规划能力尚未进入本节的已实现范围。

参考 STK 的[力模型分层](https://help.agi.com/stk/Content/hpop/hpopForceMods.htm)和[积分设置](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)，在 GCRF 中以连续 TAI 秒积分 Cowell 六维方程，RKF78 / Bulirsch–Stoer 还可选择 [VOP 通用变量方程](vop_integration.md)。输入为瞬时状态，不能把 J2/J4 平均根数当作等价瞬时初值。当前支持：

| 配置 | 实现与默认值 | 边界 |
| --- | --- | --- |
| 地球引力 | 桌面新配置默认 EGM2008、21×21，缺少数据时首次打开设置自动下载；SDK 默认内置 EGM96 8×8、最高 20×20；可下载完整 EGM96 / EGM2008，或导入静态 `.gfc` / `.grv`；文件模型计算最高 2190×2190，EGM2008 完整 2190×2159，0/0 为所选模型点质量 | 阶数与次数按文件实际范围独立限幅；支持[永久潮／完整固体潮](solid_tides.md)；支持[FES2014bv1 海潮与海洋极潮](ocean_tides.md)及独立截断；文件显式长期变化已支持；静态场默认长期演化、潮汐幅度筛选和次要分潮推算待实现 |
| 第三体引力 | 日月、七行星及冥王星独立启用，默认关闭；DE440s 地心几何位置，GM 来源为 JPL DE / Cb file / User Specified | 每个积分阶段求值；覆盖不足报错，不外推 |
| 积分器 | RKF78 / RKV 8(9) Efficient / Bulirsch–Stoer 可选固定或相对误差控制；RK4 / Gauss–Jackson 12 强制固定 | RKF78 / Bulirsch–Stoer 支持独立的 VOP 开关；其他方法保留非活动草稿 |
| 自适应容差 | 相对 1e-12，位置绝对 1e-3 m，速度绝对 1e-6 m/s | 分量误差尺度为 `atol + rtol × max(abs(old), abs(new))`；是局部数值误差，不是预报精度 |
| 步长 | 相对误差默认 0.001–60 s；固定模式默认 10 s | 输出采样间隔独立；所有固定模式不作截断误差控制 |
| 停止条件 | 最低地心高度（默认 0 m）、无法满足容差、非有限状态、资源上限 | 高度相对所选重力模型参考球面，不是大地高；不作再入事件定位 |
| 其他摄动 | 已计入球形阻力及光压；已计入永久潮及完整固体潮（可选频率项与极潮）；已接入[IERS 2003 三项相对论](relativity.md)；已计入 FES2014bv1 显式分潮和 IERS 2010 海洋极潮；潮汐幅度筛选及次要分潮推算待实现；脉冲机动使用任务序列 | STK 的完整力模型更多，不能据本阶段宣称长期 LEO/GEO 预报精度等价 |

EGM96 取自 [ICGEM 原始系数](https://icgem.gfz.de/getmodel/gfc/971b0a3b49a497910aad23cd85e066d4cd9af0aeafe7ce6301a696bed8570be3/EGM96.gfc)，完整文件 SHA-256 为 `5247a9e9c316dd2c8f8fd491d53be0e163cb5cb3676b021754240cc9e44cb43b`；使用 μ = 3.986004415e14 m³/s²、R = 6378136.3 m，参考 Lemoine et al., [NASA/TP-1998-206861](https://ntrs.nasa.gov/citations/19980218814)。`tools/data/import_egm96.py` 校验完整文件后提取 2–20 阶完全归一化系数，生成编译期表；正常构建和运行不联网。模型 ID 为 `egm96-static-tide-free-20-v1`，系数随程序固定。

球谐势采用归一化 Cartesian 角向递推与解析一阶自动微分；径向幂单独计算，最终乘入完全归一化因子。近极点使用差分形式递推，并以有理化公式计算小系数和 `u−1`，避免大项相减积累误差；逐分量补偿求和。角向递推仅保留前两阶，径向权重工作区为 O(degree)。高阶测试同时覆盖两种递推形式的切换位置、南北极及完整 EGM2008，见[验证协议](../validation/hpop_protocol.md#完整-egm2008-与-2190-阶验证)。每次计算无需经纬度除法、数值差分或未归一化阶乘。每次力评估使用当前分析持有的 EOP 将位置旋转到 ITRF，计算引力后旋回 GCRF；惯性方程不另加离心或科里奥利项。即使输出 GCRF，非中心重力也必须有 EOP，其哈希进入结果来源。数据必须覆盖初始历元到场景时段的整个积分路径；目录选源和桌面准备均按该范围检查，计算中不换源或联网。

文件模型使用自己的 GM、参考半径、潮汐系统及 `.grv` 中显式指定的实部 k20（未指定为 0.30190），静态系数保持原始潮汐系统，HPOP 按显式固体潮模式转换可分离永久项。资源保留完整文件，并将全部声明范围内的系数装入不可变 `GravityModel`；超过已经验证的 2190 阶明确拒绝。ICGEM 检查稠密系数与一致的最高次数，允许 EGM2008 的 2190 阶、2159 次尾部；不静默截掉文件内容。`HpopOptions.gravity_model` 保存目录选择 ID，`gravity_mu` 保存初始根数转换和报告所用 GM；准备时与文件核对，GM 不符时需重新选择模型并确认轨道输入。自定义文件可以更新系数，但不会偷偷重新解释原始根数。模型哈希与解析版本进入项目 LOCK，文件更新经确认后重建全部调用方，运行中的分析继续持有原资源。

步长控制在桌面单独选择 `Relative Error` / `Fixed Step`。C++ 使用 `HpopStepControl`，Python 使用 `step_control="relative_error"` / `"fixed"`。RK4/Gauss–Jackson 始终固定，其他三种方法两种模式均可使用；`uses_fixed_step()` 返回有效模式，配置字段保留切换前的草稿。只有有效参数进入分析/报告身份：固定模式忽略容差和 Min/Max Step，相对误差模式忽略固定步长；关闭的校正方式同样不参与身份。v40 / OBJS31 严格保存新枚举，旧版本拒绝。

固定单步方法使用以初始历元为原点的时间网格 `t_i = i h`，缓存块为 `ceil(300/h)` 个完整步，缓存不能把 700 s 设置改为 300 s。非网格查询的尾步和已知天气跳变可以截短当前步，之后返回原网格；尾段不回写缓存，查询顺序、取消和并发不会改变其他结果。RK4 的来源标识更新为 `rk4-fixed-grid-v2`。这些尾段是实际积分，不是科学插值。

固定步长及自适应 Min/Max Step 都支持 1e−6–86400 TAI 秒（C++ 上限常量 `hpop_max_step_seconds`）。自适应缓存段宽度为 `max(300, max_step_seconds)`，以初始历元为原点；不再用内部 300 秒缓存限制实际步长。首个试步取 `0.1 × min(r / sqrt(μ/r), r / |v|)`，再约束到配置 Min/Max，速度为零时只用引力时间尺度。该估计只决定每个计算段的首步，后续由误差控制增减；大上限不会直接作为低轨首步。误差控制、停止高度和资源覆盖仍逐阶段生效。运行时来源升级为 `steps=relative-mixed-max-span-v2`，固定模式来源保持不变。

缓存网格通过完整区间索引计算端点，避免重复加法漂移。查询不会为填满缓存读取请求区间之外的数据，尾段不回写，取消保留已经完成的整段。当前科学查询仍积分缓存尾段；大上限下密集查询可能重复计算较长尾段，尚无科学插值加速。一天固定步长只在合适的长周期数值问题中验证，不是低轨步长建议；独立对照及范围压力测试见[大步长验证](../validation/hpop_protocol.md#大步长与自适应缓存)。

显式单段工厂、普通项目及任务滑行均已接入 HPOP 接受节点或固定 TAI 输出、天气分段及 Lagrange / Hermite / VOP 科学插值；方法、阶数和输出配置贯通 UI、保存及报告，见[科学轨道星历插值](orbit_interpolation.md)。

固定 Bulirsch–Stoer 采用 2/4/6/8/12/16/24/32/48 子步序列的全部九行外推，固定十八阶；中点过程和外推均使用相对当前步起点的状态增量，降低高阶大坐标消去误差。不根据误差拒步、调整步长或选择阶数，来源为 `bulirsch-stoer-midpoint-richardson-fixed18-v2`。此固定阶数是本项目的明确选择，STK 未公开其对应内部策略；不能据此宣称与 STK 逐位相同。相对误差模式保留已验证的自动 4–18 阶与混合绝对/相对分量尺度。

[HPOP 帮助](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)明确 RKV 的嵌入误差控制；[Connect 页](https://help.agi.com/stk/SubSystems/connectCmds/Content/cmd_HPOPIntegrator.htm)的 RelativeError 行仍只列 RKF78/BS。本实现依据前者为 RKV 保留自适应并新增固定模式；STK 实机有效组合、误差范数、步长范围和默认值仍待核对。当前工程参数范围为 1e−6–86400 s，已覆盖参考配置的一天上限；这不是对 STK 全部合法范围或出厂默认值的断言，默认值保持显式配置。

RKV 8(9) Efficient 使用 Verner 的 16 阶段高效 9(8) 嵌入对，按 [STK 的八阶推进定义](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)推进八阶成员，以九阶与八阶之差估计局部误差。系数固定于 [rklib `rkv98e`](https://github.com/jacobwilliams/rklib/blob/a1bf2d26ba2c37d049b448d28d4e9800a0ee4b58/src/rklib_variable_steps.f90)（BSD-3-Clause）；其原实现推进九阶成员，OrbitForge 显式选择八阶权重。每步使用 16 次阶段力评估，所有阶段在该步闭区间内，另验证接受端点。控制器使用上述分量混合绝对/相对误差的最大范数，接受阈值 1，安全因子 0.9、缩放范围 0.1–4、指数 −1/9，随后约束到最小/最大步长。天气边界及末段可短于最小步长；若误差仍超限则报错。它与 RKF78 共用分段缓存和取消边界；固定步长模式已提供；稠密插值和 FSAL 缓存仍待实现。Python 使用 `integrator="rkv89"`，C++ 为 `HpopIntegrator::rkv89`，结果来源包含 `rkv89-efficient-8th-rklib-a1bf2d26-v1`。控制器并不宣称与 STK 内部实现相同；局部容差不能保证整段轨道误差，见[数值对照](../validation/hpop_protocol.md#rkv-89-efficient)。

Bulirsch–Stoer 的相对误差模式按 [STK 定义](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)使用 Richardson 外推和自动步长。实现采用平滑改良中点、偶数子步序列 2/4/…/18，以及关于 `1/n²` 的多项式外推，候选阶数为 4–18。最后一列外推修正使用相同分量绝对/相对尺度估计误差；第 k 行（从 0 计）的步长控制指数为 `−1/(2k+1)`。每次尝试从低阶开始，到首个满足误差要求的候选后再计算一列，以累计力评估次数 / 下一步可达到时长比较成本，在合格候选中选择；下一步受配置上限、当前计算段剩余时长和已知天气边界限制；计算段宽度至少为配置上限。最高阶仍不满足要求则缩步，在最小步长或更短边界段仍不满足要求时明确报错。所有中点阶段限制在该步时间范围内，并在子步检查取消；天气边界采用同一单侧求值规则。Python 为 `integrator="bulirsch_stoer"`，C++ 为 `HpopIntegrator::bulirsch_stoer`，来源为 `bulirsch-stoer-midpoint-richardson-4to18-cost-v1`。此选择策略和误差控制不宣称复现 STK 的内部细节；VOP 积分方程仍待接入；科学插值已接入项目，见下文。独立对照见[验证协议](../validation/hpop_protocol.md#bulirschstoer)。

Gauss–Jackson 12 使用 [STK 规定的固定步长和两种校正方式](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)。位置采用二重求和、速度采用求和 Adams，保留 13 个等距加速度值；四组预测/校正权重由 [Berry–Healy 的差分算子关系](https://hdl.handle.net/1903/2202)用有理数生成。`tools/validation/gauss_jackson_coefficients.py --check` 核对固定表、论文八阶表中的独立条目和 52 个多项式一致性条件。求和保留 SI 尺度并补偿舍入，系数点积先扣除公共加速度项。

Python 使用 `integrator="gauss_jackson"` 和 `correction="full"` / `"pseudo"`；C++ 为 `HpopIntegrator::gauss_jackson` 与 `HpopCorrection`。Full 默认开启，每次校正收敛后重算全部加速度；Pseudo 只更新所选中心场 GM 对应的二体项，保留最后一次迭代的摄动。两种方式的校正迭代均计算完整力，最多 12 次；位置/速度变化按各自三维范数的 `64 × double epsilon` 检查，使用至少 1 m / 1 m/s 尺度。未收敛明确报错，要求减小固定步长，不把迭代收敛当作截断误差保证。

历史启动以及非网格查询的尾段使用至少十四阶、最高十八阶的 Bulirsch–Stoer，采用同一 2/4/6/8/12/16/24/32/48 序列并外推局部增量以降低舍入放大，固定内部相对容差 1e−14、位置/速度绝对容差 1e−9 m / 1e−12 m/s，最小拒绝步为 1 ns。只在传播方向的已请求区间取样，不为启动要求额外前后星历。缓存间隔为 `ceil(300 / h) × h`，保存状态、求和及加速度历史；尾段副本不回写，保证查询顺序和输出网格不改变后续积分。已知天气跳变以单侧启动器过渡并清空历史；每个机动后的 HPOP 滑行创建新的历史。阴影专用事件仍待实现，非光滑力段的高阶误差不能由局部迭代收敛保证。

Gauss–Jackson 的自适应参数只保留草稿，RK 方法的校正选项同样不参与计算身份。项目 v40 / OBJS31 保存两种设置，结果来源区分 `gauss-jackson12-summed-bs14to18-startup-full-v3` 与 `gauss-jackson12-summed-bs14to18-startup-pseudo-v3`。这套启动与收敛策略是本项目明确的实现，不宣称与 STK 的内部策略相同；高偏心轨道须做固定步长收敛检查，见[数值对照](../validation/hpop_protocol.md#gaussjackson-12)。


RKF 7(8) 系数来自 Fehlberg [NASA TR R-287](https://ntrs.nasa.gov/citations/19680027281) 第 65 页 Table X，按 STK 的定义推进七阶解，以嵌入八阶解估计误差，步长控制指数为 1/8。结果算法标识为 `rkf78-7th-fehlberg-r287-v1`，避免与之前八阶推进的结果混用；公开枚举 `rkf78` 不变。不宣称逐位复现 STK 的步长控制器。直接积分工厂的自适应模式从初始历元向前后独立建立宽度为 `max(300, max_step_seconds)` 秒的完整计算段，余段单独积分；改变采样、乱序、重复和并发查询不改变结果。共享缓存加锁，取消保留已完成段，不发布部分结果。每次查询最多 200000 次积分尝试，距离初始历元最多 366 天；这是资源边界，不是精度保证。显示插值不参与科学查询。普通项目及任务滑行采用连续有界星历，Lagrange / Hermite / VOP 方法与阶数可配置；固定输出开启时使用轨道页步长，见[科学插值契约](orbit_interpolation.md)。首次查询在捕获的资源上生成完整滑行段，之后查询使用不可变插值节点。

```python
satellite = project.objects.satellite("Numerical", orbit=of.orbits.Hpop(
    initial=of.orbits.Keplerian(epoch, u.km(7000), 0.01, u.deg(98.5)),
    gravity_degree=8,             # 省略 gravity_order 时与 degree 相同
    max_step_seconds=60,
))
analysis = project.prepare(data=data)  # 显式 DataSnapshot 或 DataCatalog
trajectory = analysis.states(satellite, step=u.s(30))
```

RK4 使用 `integrator="rk4", fixed_step_seconds=10`；点质量使用 `gravity_degree=0`，启用未截断潮汐时仍需要 EOP。`satellite.orbit.hpop` 是不可变 `HpopSettings`，可用 `dataclasses.replace` 修改后将新 `Orbit` 赋回 `satellite.orbit`；旧分析继续持有旧参数与数据。完整示例见 `examples/python/propagators.py`。

C++ 设置 `OrbitInput.propagator = OrbitPropagator::hpop`、`OrbitInput.hpop`（`HpopOptions`），通过 `Project::prepare(data)` 查询。低层工厂 `runtime::make_hpop_session(initial, options, environment, id)` 持有不可变 UTC、Frame/EOP、重力、星历、空间天气和海潮 Provider 的共享所有权。核心只依赖标准库；Runtime 的 DE440s 装配需要 CSPICE。桌面选择 HPOP 后，“配置…”左侧导航分“力模型”（引力、阻力、太阳辐射压、其他）和“数值”（积分器、协方差）两组，每项下方实时显示当前草稿摘要，如“EGM2008 · 21 × 21 · 第三体 2”；窄窗口改为顶部页签。各页按标题分组，标签列对齐；补充说明收在分组标题旁的说明标记中，页面只保留限制与数据要求。阻力与光压以开关启用，关闭时只显示一行说明；光压的太阳位置方法直接显示，模型常数默认折叠为一行摘要。引力页将中心天体引力、固体潮、海潮与第三体引力放在同一滚动页面；第三体区域用 Name / Use / Source / GM 四列表格配置日月、七颗行星及冥王星，支持搜索和独立的 Cb file / JPL DE / User Specified 来源。光压页提供球形模型、独立 Cr 与面积质量比、三种阴影模式、地球／月球分配及等效大气高度；太阳位置提供 True、Apparent to True CB 和 Apparent，默认地心视太阳；遮挡体中心仍使用几何位置。最低停止高度、插值方法/阶数和固定星历输出放在积分器页。阻力页提供球形 Cd/面积质量比、JR/NRLMSISE-00、手动 F10.7/Kp 或 CelesTrak 文件（日/三小时 Ap/Kp）、低空拼接和视太阳，详见[阻力契约](atmospheric_drag.md)。“其他”页已接入 IERS 2003 三项相对论加速度，默认关闭，详见[相对论契约](relativity.md)；卫星质量默认 1000 kg，支持 kg/g/t，按 STK 球形模型语义换算对应面积而不改变各自 A/m 或加速度；地球辐射、插件及协方差尚未实现，只显示能力状态。切换内外页、调整窗口或单位不重建输入；校验失败返回相应页并定位字段。保存设置仅改变属性草稿，轨道页的“应用”才计算并发布。参数随项目 v40 保存。

独立回归使用 Orekit 13.1.2 Holmes–Featherstone 重力和 Hipparchus 4.0.2 DP853：五类轨道 × 三组阶次 × ±6000 s/±1 天共 60 个状态，检查位置每分量 0.15 m、速度 0.15 mm/s、初始加速度 5e-13 m/s²。参考刻意采用静态同轴重力系，隔离球谐与积分；真实 EOP 路由另由集成测试验证。尚无同配置 STK 导出数据直接对照，见 [HPOP 验证](../validation/hpop_protocol.md)。后续按完整截图清单继续完善光压、空间天气、大气并接入潮汐、附加力、积分高级选项和协方差；未实现的力不提供无效开关。

阻力的 `use_approximate_altitude` 可选择 WGS84 椭球径向近似，默认关闭并使用精确大地高；密度、低空拼接和高度域检查使用同一个选择。位置报告仍返回精确大地高，低层 `DragEvaluation.density_altitude_m` 可核对实际密度输入，详见[阻力契约](atmospheric_drag.md)。

## J4

`runtime/j4.hpp` 提供 `j4_mean_ellipse_state`、`j4_mean_ellipse_elements` 和 `make_j4_session`。`J4MeanElements` 与现有 `J2MeanElements` 使用同一数据布局；参数含义仍取决于选择的模型。转换函数只在辅助平均椭圆与 Cartesian 表示间转换，不进行瞬时根数到平均根数的物理拟合。

采用 Kozai 长期项：保持 a、e、i 不变，推进 Ω、ω、M；包含一阶 J2、二阶 J2 和一阶 J4。方程参考 Vallado (2013) §9.4 和 [SatelliteToolboxPropagators.jl 的 J4 实现](https://github.com/JuliaSpace/SatelliteToolboxPropagators.jl/blob/2fded936bbc911c4ecd3a48c2b9fdae7cc9c8e5e/src/propagators/j4.jl)。适用地心椭圆、近地点高于模型参考面，不含阻力、第三体、太阳辐射压和长/短周期项。

固定 EGM2008 tide-free 常数：μ = 3.986004415e14 m³/s²，R = 6378137 m，J2 = 0.0010826261738522227，J4 = -1.6198975999169731e-6。系数为未归一化形式 `Jn = -sqrt(2n+1) Cn0`，模型 ID 为 `earth-egm2008-j4-v1`。原有 J2 保留 IERS 2003 常数。

输出速度是完整长期轨迹的位置导数：包含平均角速度修正、节点进动和近地点进动，不能直接用辅助椭圆的 Kepler 速度。数值记录算法 `j4-secular-kozai-gcrf-derivative-v1`。

[STK 也定义 J4 为这些长期项](https://help.agi.com/stk/12.7.1/Content/stk/vehSat_orbitProp_2bodyJ2J4.htm)，但这不等于逐位复现 STK。[参考实现明确记录过 RAAN 的差异](https://juliaspace.github.io/SatelliteToolboxPropagators.jl/stable/man/propagators/j4/)。本实现保留公开方程的 J4 节点项符号，没有为拟合某个 STK 版本而改号；目前没有同配置 STK 导出数据的直接验收。比较时必须统一常数、平均根数定义、历元和速度约定。

## SGP4 与 TLE

`core::Tle` 保存原始两行；`core::parse_tle` 校验固定列、字符、校验和、编号一致性、范围和历元，支持 Alpha-5 编号。只接受标准 ephemeris type 0；两位年份以 57 分界，表示 1957–2056。遵循 [NORAD 格式说明](https://celestrak.org/columns/v04n03/)：第 000 天表示上一年 12 月 31 日，空白指数项表示零，整数允许前导空格。八位日小数精确换算为纳秒，不通过大浮点儒略日相减求秒差。

`providers::sgp4` 使用 Vallado 2023-05-09 算法，固定 WGS72 和 improved 模式。近地、深空、共振分支由模型自动选择，不另设“SDP4”输入类型。实现固定于 MIT 许可的 [python-sgp4 提交](https://github.com/brandon-rhodes/python-sgp4/tree/8126f773923c2902719d45aab8bdb5cce86cf39d)，C++ 原文、许可证和来源说明保存在 Provider 的 `third_party` 中。此 Provider 只依赖 Core，不依赖 Qt、CSPICE 或网络。

`make_sgp4_session(tle, time, object_id)` 必须显式注入 `ITimeProvider`，仅在初始化时把 TLE 的 UTC 历元解释为 TAI；后续使用连续 TAI 分钟差。该时间选择与 [DME/STK Components 的说明](https://help.agi.com/STKComponentsJava/Javadoc/agi-foundation-propagators-Sgp4Propagator.html)一致；跨闰秒时，与直接相减 UTC 儒略日的调用方式可能相差一秒。时间数据哈希进入 Session provenance。

原始例程会修改深空积分状态，每次查询复制已初始化记录，避免随机顺序或并发查询污染计算。初始化、衰减、非法轨道与非有限结果明确报错。TLE 原文随当前 `.orbitforge` 格式持久化；计算不自动换用另一组根数，也不在 Provider 内联网。

桌面选择 SGP4 后显示独立的粘贴/文件导入界面，可带一行卫星名称；拒绝含多组 TLE 的文件，导入失败保留当前草稿。显示编号、UTC 历元和名义周期。场景距离历元超过 14 天时提醒检查根数时效，这是交互提示，不是精度保证或模型硬有效期。切换预报器不把普通椭圆伪装成 TLE；原手动输入作为非活动草稿保留。重开 TLE 属性时，手动草稿以保存的 GCRF 状态恢复，不要求旧草稿历元仍有 EOP 覆盖。

当前尚不包含 OMM、多组 TLE 的自动选择、在线卫星目录或 TLE 拟合。数值力模型由独立 HPOP 路线推进；脉冲机动见[任务序列](missions.md)。

## TEME 与现有场景

`FrameId::teme` 表示 TEME of date，不能直接标记为 GCRF。ERFA Provider 提供 `teme_to_itrf` 和 `teme_to_itrf_state`：UT1 → GMST82 → PEF → 极移旋转（`Pom00`，s′ = 0）。状态变换包含 GMST82 的解析导数、UT1 变化率和极移变化率。GCRF 转换通过同一 ITRF 和当前 IAU 2006/2000A 模型完成。

`core::transform_state` 统一路由 GCRF、ITRF、TEME；其他参考系明确拒绝。报告、显示缓存和缓存之外的播放查询使用相同路由。SGP4 原生 Session 可不依赖 EOP；转换后的地图/三维/报告必须有覆盖查询时段的已确认 EOP。

SGP4 的约定输出速度并非其近似位置公式的精确导数。显示缓存单独以 0.125 秒位置差分建立 Hermite 切线（端点使用单侧差分，短区间缩小步长）；原生 Session 和报告继续保留标准 SGP4 速度。每段显示轨迹仍在 1/4、1/2、3/4 处验证 1 m 误差上限，显示缓存不进入科学计算。

项目 `make_orbit_session` / `make_project_scenario` 在有 SGP4 对象时要求传入时间 Provider，Python 为 `project.to_scenario(time)`。重建桌面项目时捕获已确认的时间数据，并重建 SGP4 Session；下载不会修改运行中的实例。

## 验证与限制

- 8 组 Vallado/CSSI 案例、157 个 TEME 状态，覆盖普通阻力、低偏心率、深空、低倾角、半日/同步共振、负时间；容限 1 mm、2 μm/s，由原始输出精度和平台浮点差异确定。
- J4 使用 60 位精度独立参考程序求解 Kepler 方程并对位置求导，验证 ±1 / ±30 天状态；另测零谐项二体极限、J2² 保留、圆/赤道/极轨/高偏心率和非法输入。参考生成器及固定值见 `tests/reference/propagators`。
- TEME 对照 Vallado 2006-6753 Rev 2 Appendix C。公开例使用单个 double 儒略日，造成约 22.790 μs 的 UT1 舍入和 13.2 mm 的位置残差；精确打印 UTC 的 60 位计算与本实现相差小于 1 μm。速度与原例存在 GMST 导数/恒定地球自转率约定差异，比较容限 0.2 mm/s；另验证数值导数和完整状态往返。
- 当前格式固定样本、往返读写及非当前版本拒绝；无时间/EOP、损坏 TLE、未应用草稿、并发与衰减错误路径。

这些检查验证实现一致性，不代表 TLE 的真实预测误差达到毫米级。实际误差取决于根数质量、历元距离、阻力和机动。

第三体配置、系统质心约定及 Cb 文件导入范围见[第三体引力](third_body_gravity.md)。


HPOP `include_secular_variations` 使用所选重力资源的显式参考历元和线性变化率，每个积分阶段重新求值，并与固体潮、海潮的系数修正相加。非中心重力启用该项而文件没有相应元数据时明确失败；0/0 保留非活动草稿。长期变化仅使用 TAI/TT 连续时差，不额外要求 UTC 或星历。静态 EGM 产品暂不按名称推断默认演化参数；完整语义与当前范围见[引力系数长期变化](gravity_secular.md)。

时间正则化已贯通 C++ / Python、项目保存及 Integrator 页：开启后以指数和每圈步数设置 Sundman 时间，固定秒步长停止生效；初始轨道须为非径向束缚椭圆。相对误差模式的 Min/Max Step 仍为物理秒；精确输出、天气跳变及机动重启见[时间正则化](time_regularization.md)。
