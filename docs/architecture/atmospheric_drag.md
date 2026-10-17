# HPOP 大气阻力

桌面阻力页、C++ `DragOptions` 和 Python `orbits.DragSettings` 共用 Core 模型。当前实现球形阻力、Jacchia–Roberts / NRLMSISE-00 密度、手动/文件空间天气、低空拼接、近似高度及地心视太阳位置；低层密度接口支持原生日 Ap 和七项历史 Ap；自然三次样条、保持窗口均值插值、CelesTrak 月预报及 CSSI Predicts 已接入；风场尚未实现。[STK 阻力设置](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)是功能与输入含义的对齐依据；尚未取得 STK 同配置数值输出。

## 力与参考系

在 ITRF 中计算 `a = -0.5 * rho * Cd * (A/m) * |v_rel| * v_rel`，单位为 kg/m³、m²/kg、m/s、m/s²。大气与地球共转且无风；`v_rel` 来自完整 GCRF→ITRF 状态变换，包含旋转率项。只旋转惯性速度会错误地把地球自转计入相对速度。每个积分阶段重新求 UTC、EOP、太阳和密度，再将加速度旋回 GCRF。

默认密度高度使用精确 WGS84 椭球高，半长轴 `a=6378137 m`、扁率 `f=1/298.257223563`。`use_approximate_altitude=True` 启用 [STK 文档中的椭球径向近似](https://help.agi.com/stk/Content/hpop/approximateAltitudeComputation.htm)：`h=R−a(1−f)/sqrt(1−f(2−f)cos²φ)`，其中 `R` 是地心距，`φ` 是地心纬度。赤道和两极采用其有限极限；没有把大地纬度代入该公式。

开关只替换密度、模型有效高度检查和高低空拼接使用的高度；大地经纬度保持精确定义，位置报告仍为精确 WGS84 大地高。`DragEvaluation.geodetic` 返回精确经纬高，`density_altitude_m` 单独返回实际密度高度。当前仍计算精确纬度，因此不承诺此开关带来 STK 所述的性能收益。它不改变积分器的最低地心高度阈值。太阳赤纬与当地太阳时由同一 ITRF 太阳矢量求得，太阳时角是向东相对太阳子午线的夹角。地心奇点、非有限输入和域外高度明确报错。

Cd 为带符号经验系数，范围 −100…100；面积质量比为 0…9999 m²/kg，与光压参数独立。这两项范围对应 [STK HPOP Drag 命令](https://help.agi.com/stk/12.10.0/Subsystems/connectCmds/Content/cmd_HPOPDrag.htm)。负 Cd 产生反向经验加速度；正常阻力使用正 Cd。零面积或零 Cd 不改变已启用模型的数据依赖。

## 密度与天气

Jacchia–Roberts 适用 90…2500 km，保留 NASA GMAT 标量模型的拟合常数及分段公式，并处理正午、午夜、极点、太阳赤纬为零的有限极限。GMAT 外层 `Density` 不开放 90…100 km，本实现使用其原有标量公式开放这一高度段。NRLMSISE-00 适用 0…1000 km，移植 Orekit 13.1.2 的双精度计算和表格，使用 `gtd7d` 总质量密度，包含异常氧；默认变化项开启，使用日 Ap。源码来源和 Apache-2.0 许可见 [模型说明](../../modules/core/src/propagation/atmosphere/README.md)。所有临时状态属于单次调用，模型可并发使用。

手动天气来源为常量输入：日 F10.7、平均 F10.7（sfu）、Kp。NRLMSISE 将手动日值作为前一日 F10.7，将平均值作为模型平均通量；没有隐含的历史查询。Kp 使用标准 1/3 刻度的 28 项 Ap 表，非表格刻度线性插值，Kp=3 对应 Ap=15。界面/API 当前限制通量 `(0,1000]` sfu、Kp `[0,9]`，这些限制与 STK 命令允许的全部范围不同，不能据此宣称所有参数范围已对齐。低层 `AtmosphereInput` 使用 UTC 公历日期 1600…2600；闰秒合法性在 HPOP 中由实际时间 Provider 认证，独立标量调用方负责保证其 UTC 真实有效。

JR 可选择 NRLMSISE-00 作为低空模型。低于 90 km 使用低空模型；在 `90 km…90 km+拼接范围` 内，以 `w=t²(3−2t)` 对质量密度加权，在其上使用 JR。正范围拼接具有连续的一阶权重；零范围表示明确的硬切换。有效拼接范围至多 910 km，防止低空模型查询超出 1000 km。NRLMSISE 为主模型时低空设置不生效，但合法草稿仍保存。该拼接定义为 OrbitForge 的公开约定，仍需 STK 数值核对。

JR 的高空温度比值使用直接指数求值，避免 `T∞−T` 相减后丢失有效数字或得到零密度；模型常数和分段保持原定义。新增 C++ [阻力偏导接口](drag_differential.md)，用于后续协方差和 D 参数敏感度传播；其密度梯度和模型边界限制有独立契约。

## 原生 Ap 密度接口

`evaluate_atmosphere(NRLMSISE00, input)` 可通过 `AtmosphereInput.daily_ap` 直接使用原生日 Ap，或通过 `ap_history` 使用七项历史数组；二者互斥，均省略时沿用 Kp→日 Ap。显式 Ap 模式不读取 Kp。所有 Ap 值必须有限且在 `[0,400]` 内，JR 拒绝显式 Ap，避免误把 Ap 当成 Kp。

历史数组按模型原生顺序保存：日 Ap、当前三小时 ap、此前 3 / 6 / 9 小时 ap、此前 12–33 小时八项 ap 的均值、此前 36–57 小时八项 ap 的均值。输入归调用方所有，模型按值使用；每次计算的状态独立。日模式设置 `SW[9]=1`，历史模式设置 `SW[9]=-1`，即使七项相同也不能把历史模式简化为日模式。原生定义见[原作者接口](https://git.linta.de/~brodo/nrlmsise-00.git/plain/nrlmsise-00.h?id=bc9a2feba4344e74201281e563332688a4d09cc3)。

```python
inputs = of.AtmosphereInput()
inputs.utc = of.UtcDateTime(2000, 2, 29, 12, 30, 0)
inputs.geodetic = (0.4, 0.6, 400_000)  # 经度、大地纬度、椭球高；rad、rad、m
inputs.f107, inputs.f107_average = 150, 150
inputs.ap_history = (40, 400, 100, 50, 20, 7.125, 3.75)
density = of.evaluate_atmosphere(of.AtmosphereModel.NRLMSISE00, inputs)
```

这个入口也供空间天气装配调用；标量密度接口本身不读取文件、不补齐缺值。HPOP 的文件模式在每个积分阶段通过不可变 `SpaceWeather` 取样；JR 与低空 NRLMSISE 分别使用自己的时间滞后。

## 空间天气文件

支持 CelesTrak 原始 31 列 CSV 和 130 列 `CssiSpaceWeather` 定宽文件，格式依据 [CelesTrak 说明](https://celestrak.org/SpaceData/SpaceWx-format.php)。原始全文及 SHA-256 保留，解析区分观测/预报、观测/1 AU 调整通量、中心/尾随 81 日均值、日 Ap 与三小时 ap。空字段保持缺失。观测 Kp 的 53 表示 5⅓；预报中由 Ap 推导的非三分之一值（如 22、24）按十分之一刻度保留。

桌面“阻力 → 太阳通量 / 地磁”可选手动或文件、每日、三小时、三小时保持均值插值或自然三次样条更新，以及 Ap 或 Kp 来源。文件由数据中心下载/导入 CelesTrak `SW-All.csv`，API 不隐式联网。`DragSettings.weather_model` 是目录选择 ID，空字符串表示手动，`geomagnetic_update` 为 `daily` / `three_hourly` / `three_hourly_interpolated` / `three_hourly_cubic_spline`，`geomagnetic_source` 为 `ap` / `kp`。也可显式 `DataSnapshot.with_space_weather("celestrak", path)`。

按照 [STK Drag 文件说明](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)，日与中心 81 日观测通量在 20:00 UTC 节点间线性取样。滞后采用 [AGI JR 定义](https://help.agi.com/STKComponents/html/T_AGI_Foundation_Celestial_ScalarDensityJacchiaRoberts.htm)及 [MSIS-2000 定义](https://help.agi.com/STKComponents/html/T_AGI_Foundation_Celestial_ScalarDensityMsis2000.htm)：两者日通量滞后 86400 s；JR 平均通量也滞后 86400 s、地磁滞后 24120 s；NRL 平均通量及地磁不滞后。这是公开组件文档依据，尚未完成 STK HPOP 同场景数值验收。

积分步在日/三小时地磁更新处截断（JR/低空 NRL 各自的滞后窗口均计入），RK 端点使用该步内部一侧的天气值；正反向均不跨跳变混合密度。滞后按 TAI 实际秒数，日/三小时窗口按 UTC；正闰秒属于当天最后一个三小时窗口。NRL 三小时模式组装上述七项 Ap 历史；选择 Kp 时先逐项转换为 Ap 再求历史均值。日模式使用文件日 Ap 或由文件日 Kp 转换，不能从当日八项重新计算并替换原始日值。

准备时检查完整传播路径所需的每个日期、通量插值节点和全部历史槽，包括初始历元早于场景的情况。缺字段、内部断档、域外查询均报错，不外推或零填充。月预报采用下述独立的月节点取样，原始首末日期不是可传播范围。FXM 与 CSSI Predicts 范围见对应章节。

## 三小时自然三次样条

`three_hourly_cubic_spline` 对应 STK 的 3-Hourly Cubic Spline 选择。STK 公开说明采用自然三次样条，但没有公开节点落点、断档端点和越界策略。以下是 OrbitForge 的明确计算约定，已对照独立 SciPy 与 GMAT；这些细节仍需同配置 STK 数值验收，不将其称为完全复现。

将每个三小时值放在该 UTC 窗口中心（01:30、04:30、…、22:30）。在连续、非月预报且该来源无缺值的序列上，一次性求自然三次样条，首末节点的二阶导数为零；两节点退化成线性，孤立节点仅允许精确命中。Ap 与 Kp 各有自己的有效序列，缺失日期/字段和月预报均断开序列，不跨缺口连接，也不外推。远处节点可能影响同一完整序列的曲线，因此更新文件仍须重新准备和确认；运行中的数据不可变。

样条的独立坐标是等距 UTC 三小时窗口编号。每对节点间以真实 TAI 时差求局部分数，跨闰秒的节点间隔实际为 10801 s。该时间映射连续，样条对窗口坐标 C²；不承诺闰秒处对 TAI 的导数连续。JR 仍先施加 24120 s 地磁滞后。选择 Ap 时先插值 Ap，再转为 Kp；选择 Kp 时直接插值 Kp，不能先转换节点再声称曲线相同。

自然样条可能越过原始节点范围。结果超出 Ap `[0,400]` / Kp `[0,9]` 时明确报错；仅容忍不超过 `64 × double epsilon × 上限` 的舍入偏差并收回边界。准备计算逐段检查端点及导数的实根，能发现两端合法而内部越界的情况。错误提示可改用每日/三小时取样或完整数据，不静默改变已选插值算法。

依照 STK 对 MSIS 不使用插值的说明，NRLMSISE 在该选择下沿用未插值的七项 Ap 历史、`SW[9]=-1`，其有效身份等同三小时取样。JR 与低空 NRL 混合时，两者仍分别取样；前者使用样条，后者使用原生历史。设置草稿保留选择，手动天气或禁用阻力时该项不改变结果身份。现有 UTC 步边界策略仍生效。

C++ / Python 的枚举新增 `three_hourly_cubic_spline` / `THREE_HOURLY_CUBIC_SPLINE`；高层 `DragSettings` 使用同名字符串。项目 v40 / OBJS31 保存自然三次样条枚举 3，拒绝旧格式和未知值；参考样例独立构造并覆盖未启用草稿。JR 文件样条的算法标识包含 `natural-utc-midpoint-runs-v1;overshoot=error`。

## 三小时保持均值插值

`three_hourly_interpolated` / `THREE_HOURLY_INTERPOLATED` 对应 [STK 的 3-Hourly Interp](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)：每个三小时窗口内的平均值等于原始 Ap/Kp 记录。它与自然三次样条是不同选项。STK 未公开完整边界与限幅算法；下面是 OrbitForge 可复现的实现约定，同配置 STK 数值对照仍待验收。

对每个连续有效序列，在 UTC 三小时窗口边界拟合累积积分的自然三次样条，再对其求导，得到每个窗口的二次多项式。该[积分函数构造方法](https://kluge.in-chemnitz.de/opensource/spline/)自然保留窗口均值；实现直接求解相邻均值差的三对角方程，避免大累积量相减。每个来源独立分段，缺日期、缺字段和月预报均切断序列；单窗口退化为常值，首末窗口内可查询，窗口外不外推。

设窗口均值为 `y`，原始二次曲线为 `p(u)`，`u∈[0,1]`。检查两个端点与内部顶点；若超出 Ap `[0,400]` / Kp `[0,9]`，采用 `y + θ(p(u)−y)`，取满足上下界的最大 `θ∈[0,1]`。这是[保留单元均值的缩放限制方法（式 5.3）](https://archive.ymsc.tsinghua.edu.cn/pacm_download/113/1168-paper.pdf)，这里对整个二次区间的极值施加约束。均值为零或上限时为常值。它保持每个窗口的积分；逐点截断则不具备这一性质。未限制的曲线在均匀窗口坐标下 C¹；限制可能在窗口边界产生跳变，沿用已有单侧天气取样和积分步截断。

窗口按 UTC 的 00:00、03:00、…、24:00 划分，采用左闭右开区间；局部坐标使用该窗口实际 TAI 时长，因此包含正闰秒的 10801 s 窗口也保持时间平均值。JR 施加 24120 s 地磁滞后，再对所选 Ap/Kp 插值，最后才转换指数；均值保持针对所选原始量，非线性转换后的量不声称均值相等。NRLMSISE 主模型和 JR 的低空 NRL 分支均沿用原生七项 Ap 历史、`SW[9]=-1`；主模型 NRL 的有效身份等同 3-Hourly，设置草稿仍保留所选项。

桌面按 Daily、3-Hourly、3-Hourly Interp、3-Hourly Cubic Spline 排列。项目 v40 / OBJS31 将四项分别保存为 0、1、2、3，拒绝所有非当前版本。JR 文件模式算法标识包含 `geomag-interp=natural-primitive-quadratic-utc-runs-v1;bounds=cell-mean-scaling`。Core 无新增外部依赖；SciPy 只用于独立参考生成。

## STK FXM 文件

导入器支持 STK `stkFluxGeoMag` 的 `OBSERVED`、`F10_PREDICT`、`AP_PREDICT` 三个定宽段，按此顺序读取，保留原始字节及 SHA-256。日期在每段内严格递增，预测不得覆盖观测；F10.7 和 Ap 预测按日期合并。每段必须完整结束；缺字段保持缺失。观测 Q=3 表示无太阳通量观测，不能将该数值当作有效日通量。格式依据 [STK FXM 说明](https://help.agi.com/stk/Content/stk/example_SolarFlux2.htm)。

FXM 只有 1 AU 调整通量。`SpaceWeatherInfo.solar_flux_adjusted` 明确此来源；取样在每个所需的 20:00 UTC 节点调用几何日地距离，将日 F10.7 和文件给出的中心 81 日均值分别乘以 `(149597870700 / distance_m)^2`，再执行现有线性插值与模型滞后。文件中已经计算的 81 日均值直接使用，不自行重新求平均。CelesTrak 文件始终使用自己的 observed 字段；不因缺失而自动改用 adjusted 字段。STK 文档确认 [FXM 的距离改正及 20:00 UTC 插值](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)，但没有公开全部改正实现细节；这里的节点改正约定仍待同配置 STK 实测验收。

低层 C++ / Python `sample`、`validate_interval` 增加可选 `sun_distance_m(Epoch) -> metres` 参数。FXM 要求该函数；其他文件不调用它。回调必须来自本次计算冻结的几何星历，距离须有限且为正。HPOP 自动使用已经捕获的 Sun 星历，独立于密度的“Use Apparent Sun Position”方向选择；不访问网络。准备阶段检查包含模型滞后在内的每个必需通量节点，故星历仅覆盖传播起止时刻仍可能不足。换算结果超出模型 `(0, 1000] sfu` 输入范围时明确失败。

观测段的八个 Ap/Kp 支持现有每日、三小时、保持均值插值及自然样条取样。FXM 预测段只提供日 Ap，支持 **Daily + Ap**；不伪造三小时值或 Kp，缺少所选模式需要的字段时给出覆盖错误。分开的预测段存在日期缺口也不补值或外推。桌面数据中心的天气导入支持 `.fxm`；自动下载仍使用 CelesTrak CSV。FXM 数据解释标识增加 `flux=adjusted-au-geometric-utc20-v1`，原始天气与冻结星历都参与项目数据引用；当前项目格式为 v40 / OBJS31。

## 月预报与 CSSI Predicts

CelesTrak 的 `PRM` / `MONTHLY_PREDICTED` 记录是月初节点；通量与文件提供的中心 81 日均值在各月 1 日 20:00 UTC 之间线性插值，日 Ap/Kp 在月初 00:00 UTC 之间线性插值。字段仍独立保留缺失状态；例如官方月预报缺少 Ap/Kp 时，不能用零或固定常量代替。取样需要的指数或中心均值缺失即报错。日期语义依据 [CelesTrak 格式说明](https://celestrak.org/spacedata/spacewx-format.php)。

导入器另支持 [CSSI Predicts `.dat`](https://help.agi.com/stk/Content/stk/importfiles-15.htm)：七个整数的文件头及六列月记录，逐项检查日期、数量、首末月份、有限值和物理范围。按 STK 格式将节点放在每月 15 日；使用名义 F10.7、名义 Ap，原始二倍标准差列随文件完整保存，当前 HPOP 不应用随机扰动。该格式强制使用 Ap，桌面自动选择并禁用 Kp；C++ / Python 即使收到合法 Kp 草稿也按 Ap 计算，有效计算身份记录 Ap。文件头生成月份不改变节点日期。

CSSI 没有单独的平均通量列。当前约定先在每个 UTC 日期的 20:00 对月通量曲线取样，使用当天及前后各 40 个日节点的算术平均，再对相邻日平均节点线性插值。F10.7 使用文件名义值，不附加 FXM 的日地距离改正。[STK GenFluxHistory](https://help.agi.com/stk/Subsystems/connectCmds/Content/cmd_GenFluxHistory.htm)说明了中心 81 日平均和历史数据需求，但未公开 HPOP 直接读取月预报的全部算法；本节的平均构造、地磁节点时刻和边界约定须继续用 STK 同配置结果验收。

月曲线的局部分数使用两个节点间的实际 TAI 时长，计入月长、闰日与闰秒。连续月节点必须相隔一个公历月且日期一致；允许最后一个日记录连接下个月的首个月节点。缺月不跨越，首末节点之外不外推。最后节点只允许精确命中；整个准备区间还需满足通量滞后、81 日平均和历史 Ap 的额外覆盖要求，不重复末尾太阳活动周期。

JR 的四种更新选择在月预报段均使用连续月曲线，经模型地磁滞后后求值；观测段仍使用所选的日/三小时算法。NRL 的 Daily 使用月曲线当前 Ap，三种三小时选择使用七项历史模式：在各实际时间滞后处取月曲线值，转换所选指数后再求两组八项平均。月曲线只提供预报取样，不向原始记录伪造八个观测槽；跨观测/预报边界的历史按各段自己的取样方式组装。

数据中心支持导入 `.dat`，`SpaceWeatherInfo.cssi_predictions` 和目录重开后的元数据一致。所有天气数据解释标识升级为 `spaceweather-utc20-linear-jr-msis-lags-monthly-linear-v2`；CSSI 额外记录 `cssi=nominal-ap-only;average=centered-81-utc-days-v1`，FXM 保留原改正标识。项目结构仍为 v40 / OBJS31；旧天气解释引用触发既有数据变化确认，已准备分析继续持有原不可变数据。独立 SciPy / NumPy / GMAT 对照见[验证协议](../validation/atmospheric_drag_protocol.md)。

## 数据与持久化

阻力即使只启用点质量引力，也需要覆盖完整传播路径的 UTC、EOP、Frame Provider 和太阳星历。启用后不因缺项而退回无阻力；UTC 内容哈希必须与 EOP 保留的时间来源一致。数据快照保留这些 Provider 的共享所有权，编辑项目或更新数据不改变已准备的计算。

默认使用几何太阳。`use_apparent_sun_position=True` 使用 CSPICE 对地球接收观测的 `LT+S` 太阳方向；此约定影响密度中的当地太阳时/赤纬，不修改第三体引力或光压的几何太阳。准备计算时额外认证区间起点前 600 s 的太阳与地球核覆盖，边界不足明确拒绝。自定义星历必须提供相应查询和区间认证接口，不能将几何方向冒充视方向。

项目格式为 v40 / OBJS v31，只读写当前版本。禁用模型时保存合法参数草稿，计算身份忽略这些草稿；启用时计入实际密度模型、天气、Cd、面积质量比、高度算法、太阳方法及 UTC/EOP/星历来源。NRLMSISE 主模型的低空草稿和未启用的拼接范围不改变结果身份。

```python
import orbitforge as of

drag = of.orbits.DragSettings(
    enabled=True, atmosphere="jacchia_roberts", cd=2.2,
    area_mass_ratio_m2_per_kg=0.02,
    low_altitude_model="nrlmsise00", blending_range_m=30_000,
    f107=150, f107_average=150, kp=3,
    use_apparent_sun_position=True, use_approximate_altitude=True,
)
orbit = of.orbits.Hpop(initial=initial, drag=drag)
# data 必须显式提供覆盖传播区间的 UTC、EOP、引力和太阳星历。
satellite = project.objects.satellite("Satellite", orbit=orbit)
analysis = project.prepare(data=data)
```

独立密度、传播、覆盖和跨接口验证见[阻力协议](../validation/atmospheric_drag_protocol.md)。现有状态报告直接查询同一阻力 Session；密度/分项加速度专用报告尚未实现。

## 共享卫星质量

`HpopOptions.satellite_mass_kg` / `HpopSettings.satellite_mass_kg` 默认为 1000 kg，必须有限且正。“其他”页支持 kg、g、t；阻力和光压页的面积质量比仍是独立输入，并显示只读的对应面积。C++ `drag_area_m2()` / `srp_area_m2()`（Python 同名只读属性）返回各自 A/m 与质量的乘积，拒绝溢出或非零乘积下溢为零；零 A/m 允许零面积。

修改质量只改变对应面积，球形加速度、轨迹与计算身份保持不变；设置仍写入项目并标记为已修改。未来非球形或插件模型使用质量时，必须把实际使用的质量纳入身份。此行为依据 [STK Satellite mass](https://help.agi.com/stk/Content/hpop/hpopAdditional.htm)，独立面积/质量模式不作为球形模型的第二套输入。
