# HPOP 与 STK 的对照及重构要求

本文定义 OrbitForge 通用地球轨道数值传播的目标契约与验收顺序，以 STK 的公开设计和参数语义为准。Gravity、Drag、SRP、Additional、Integrator、Covariance 六页是组织依据，包含未勾选、灰显和子窗口中的目标设置；不能仅实现截图当前启用的组合。后续力模型截图只作辅助参考，不据此引入与 STK 不同的输入模式。交付按下表逐项验收。

当前已实现静态重力资源、文件显式长期变化、永久/完整固体潮、FES2014bv1 显式分潮与海洋极潮、太阳系第三体引力、球形阻力/太阳辐射压及三项相对论：数据中心下载 EGM2008 / EGM96 / DE440s、导入外部静态重力文件，最高 2190 阶重力计算、日月与行星差分引力，以及无阴影／圆柱／双锥和地月／行星球面遮挡配置，贯通桌面、C++、Python 和项目数据引用。其余目标按下文逐项验收；当前能力以[预报器](propagators.md#hpop)为准，不能把本增量称为完整 HPOP。

桌面第一次正常启动必须准备 UTC、C04、finals2000A、完整 EGM2008，以及启用 CSPICE 时的 DE440s；已有有效文件直接复用，缺失文件由启动准备窗口下载。进度、取消、校验与原子安装复用数据中心；失败可重试，取消则退出启动。新建 HPOP 使用 EGM2008、21×21，配置中不显示“已安装”。以后所选官方重力文件缺失时，HPOP 设置页仍提供下载与重试。内置示例和点质量可主动选择；计算 API 的资源装配保持显式。

对照依据是 STK 13.1 官方帮助，以及维护者提供的 Gravity、Drag、SRP、Additional、Integrator、Covariance 六个设置页面。截图中的参数是一组参考配置，不据此推定所有 STK 版本的出厂默认值。HPOP 的公开接口和模型说明可供对照，不能据此声称复现了其内部源码、步长控制器或全部精度特性。

本轮已接入球形阻力、JR / NRLMSISE-00、手动/文件天气、低空拼接、近似高度和阻力地心视太阳；桌面、C++、Python、保存重开和状态报告使用同一实现，详见[阻力契约](atmospheric_drag.md)。低层 NRLMSISE 密度接口已支持原生日 Ap 与七项历史 Ap，并通过原作者模型验证；HPOP 已接入 CelesTrak 文件、日/三小时更新、Ap/Kp 来源和历史窗口；三小时自然样条已接入（节点/端点约定待 STK 验收）；保持窗口均值插值及 FXM 已接入；CelesTrak 月预报及 CSSI Predicts 名义预报已接入并与 SciPy / NumPy / GMAT 对照，月节点/平均构造及 STK 同配置数值验收仍待完成。

## 1. 重构起点

| 领域 | OrbitForge 0.2.1 实际能力 | 重构要求 |
| --- | --- | --- |
| 中心引力 | 编译期 EGM96，默认 8×8，最高 20×20；常数固定 | 可选 EGM2008 / EGM96 / 外部重力文件，模型自己的 μ、参考半径、潮汐系统及阶次上限 |
| 时变重力 | 无 | 区分长期变化、永久潮、完整固体潮、海潮及极潮，避免重复计入永久潮 |
| 第三体引力 | 无；已有 CSPICE 接口没有接入 HPOP | 日月及可选行星；星历和 GM 来源独立明确，使用差分引力 |
| 大气阻力 | 无 | 大气密度模型、Cd、面积质量比、空间天气、相对大气速度和高度约定 |
| 太阳光压 | 无 | 球形模型、Cr、面积质量比、无阴影 / 圆柱 / 双锥、地月遮挡、太阳位置方法与阴影边界处理 |
| 附加模型 | 无 | 相对论、地球反照与热辐射；明确的自定义力接口 |
| 积分 | GCRF Cowell、RKF78 八阶推进、RK4；固定 300 s 缓存段 | 独立的方程形式、积分算法、步长控制、结果输出与插值；逐项验证而非只扩充枚举 |
| 协方差 | 无 | 状态转移矩阵、6×6 协方差、坐标变换、参数敏感度及 consider analysis |
| 数据与来源 | HPOP 环境只有 Frame / EOP；分析快照只有时间 / EOP | 重力、星历、空间天气及潮汐资源统一管理，参与覆盖检查、项目引用与结果身份 |
| 数值验收 | 固定同轴重力场下的 Orekit 参考，另测真实帧转换路由 | 真实旋转地球下的分项力、组合轨迹、阴影事件与协方差对照；增加同配置 STK 实测参考 |

对应实现位置：`modules/core/include/orbitforge/core/propagation/hpop.hpp`、`modules/core/src/propagation/hpop.cpp`、`modules/core/src/propagation/earth_gravity.cpp`、`modules/runtime/src/analysis.cpp`、`apps/desktop/src/viewer/hpop_settings.cpp`。当前测试是已有重力实现的回归依据，不能扩展解释为完整 HPOP 的验收结果。

### 维护者参考配置

六个截图提供以下具体配置，后续用于检查编辑器表达能力和配置往返。禁用的子项按草稿保存，不作为已启用模型计算。

| 页面 | 截图中的生效配置 |
| --- | --- |
| Gravity | EGM2008，21×21，启用长期变化；Permanent tide only，截断到重力阶次；海潮关闭；Sun / Moon 开启，GM 来源为 Cb file |
| Drag | Spherical，Cd=2.2，A/m=0.02 m²/kg；Jacchia–Roberts；不叠加低空模型；手动 F10.7=150、平均 F10.7=150、Kp=3；近似高度与视太阳位置开启 |
| SRP | Spherical，Cr=1，A/m=0.02 m²/kg；Dual Cone；边界修正关闭；Apparent To True CB；等效大气高度 0 km；遮挡体为 Earth / Moon |
| Additional | 反照、热辐射、相对论、插件均关闭；卫星质量 1000 kg |
| Integrator | RKF7(8)，VOP 关闭；相对误差 1e-13，步长 1–86400 s；Lagrange 7 阶插值；时间正则化关闭；固定步长输出开启；停止高度 10 km |
| Covariance | Compute Covariance 关闭；页面仍应能表达初始矩阵、参考系、独立重力截断和 consider 参数 |

当前已支持 EGM2008、21 阶及日月引力，但仍不能完整表达这组配置：缺少潮汐及光压高级设置；Cb 常数文件仅导入 GM 子集，全部位置使用 DE440s；固定/自适应步长已支持至 86400 s；科学插值及独立 VOP μ 已接入，VOP 通用变量积分方程已接入；协方差配置仍缺失。重构不能只把数值上限放宽，必须随相应模型和收敛验证一起交付。

截图未提供初始状态、分析时段、实际重力系数文件、Cb 常数文件、星历、EOP 与参考轨迹，因此它是配置参照，而非数值真值。正式比较应从实际场景导出精确参数和资源，不用截图中显示的舍入数值代替它们。

### 后续参考图与实现顺序

维护者已明确以 STK 设计为准，后续截图只作参考；本机没有 STK，WGS84 球谐系数模型暂缓。WGS84 椭球高度已支持，不代表同名球谐重力模型已经实现。球形阻力与光压继续各自输入 A/m，共享质量放在 Additional；手动天气按 STK 输入 Kp，第三体沿用 Name / Use / Source / Gravity Value 表。

| 顺序 | 功能 | 当前状态 |
| --- | --- | --- |
| 0 | 解除 360×360 限制 | 支持至 2190×2190，完整 EGM2008 为 2190×2159；全阶 Orekit 参考、极点解析值及接口往返已通过 |
| 1 | Additional 卫星质量 | 已接入 kg/g/t、独立阻力/光压对应面积、C++/Python 和保存重开；球形加速度仍仅使用显式 A/m；以 Orekit 面积与质量参数独立验收 |
| 2 | Gravity 时变系数与剩余潮汐设置 | 按六页清单继续；文件自定义 Love 数、FES2014bv1 显式分潮与海洋极潮已接入；文件显式长期变化已接入，静态 EGM 默认演化、幅度筛选和次要分潮推算继续逐项核对、实现和验证 |
| 3 | 阻力、光压、其他及数值页的剩余设置 | 按下表逐项补齐，保持 STK 参数含义和成熟算法对照 |
| 暂缓 | WGS84 球谐重力模型 | 等待可核实的 STK 原始系数与版本；不以 WGS84 椭球或 EGM84 冒充 |

### 六页逐项验收清单

“部分”表示已有真实能力，但还不满足该项完整要求；“待实现”不是取消范围。每行完成均须有计算语义、资源依赖、API、项目往返、UI、独立参考和失败路径，不能仅增加控件或枚举。插件的目标是 OrbitForge 自定义力模型接口，不承诺直接加载 STK 的 COM 二进制插件。

| 页面 | 必须支持的项目 | 当前状态与验收重点 |
| --- | --- | --- |
| Gravity | 重力文件；Maximum Degree / Order | 已接入：EGM2008 完整 2190×2159、EGM96 360×360、外部静态文件最高 2190×2190；阶数和次数按实际文件独立限幅；全阶 Orekit 加速度与正反向轨道对照 |
| Gravity | Include Secular Variations | 部分：文件驱动的 TT 参考历元、按儒略年的归一化变化率和开关已贯通计算、UI、Python、项目与来源；静态 EGM96/EGM2008 缺少元数据时的 STK 默认演化仍待核对，不按模型名补值。见[长期变化契约](gravity_secular.md) |
| Gravity | 固体潮模式，含 Permanent tide only | 部分：三种模式、日月形变、71 个频率项和 IERS 2003 固体极潮已接入，永久潮按底场去重；`.grv` 自定义 k20 已接入并与 Orekit 对照，见[固体潮契约](solid_tides.md) |
| Gravity | Truncate to Gravity Field Size；Time Dependent Terms；Min. Amplitude | 部分：阶/次独立截断及附加时间项开关已生效，可扩展超出底场阶次；幅度阈值待实现 |
| Gravity | Use Ocean Tides；独立 Degree / Order；Min. Amplitude | 部分：FES2014bv1 官方 100×100 / 36 个显式分潮、独立阶次、IERS 2010 海洋极潮和统一文件管理已接入并通过 Orekit 对照；Min. Amplitude 与 STK 次要分潮推算待实现，见[海潮契约](ocean_tides.md) |
| Gravity | 第三体 Name / Use / Source / Gravity Value | 已接入：日月、七行星及冥王星；三种 GM 来源、Cb 常数导入和中心/系统质心约定。仍需同配置 STK 实测，且不导入 Cb 文件的其他星历设置，见[第三体契约](third_body_gravity.md) |
| Drag | Use Drag；Spherical；Cd；Area/Mass Ratio | 已实现：共转 ITRF 大气相对速度、完整旋转率项、独立面积质量比及 Cd；见[阻力契约](atmospheric_drag.md) |
| Drag | Atmospheric Density，含 Jacchia–Roberts | 已实现 JR / NRLMSISE-00 gtd7d；分别核对原始 GMAT / 作者 C 密度，尚未完成 STK 同配置数值验收 |
| Drag | Low Altitude Density；Blending Range | 已实现 JR 低空 NRLMSISE-00、正范围 smoothstep 拼接和显式零范围硬切换；STK 拼接细节仍需数值核对 |
| Drag | Solar Flux/GeoMag 来源；Daily / Average F10.7；Kp | 部分：手动日/平均通量、Kp→Ap 表与插值已生效；CelesTrak CSV/定宽文件、每日/三小时 Ap/Kp、模型滞后与完整历史检查已接入；自然三次样条与 SciPy/GMAT 对照已接入，节点/端点的 STK 细节待验收；保持窗口均值插值已实现并与 SciPy/GMAT 对照；CelesTrak 月预报与 CSSI Predicts 名义 Ap、中心 81 日平均已接入并与 SciPy/NumPy/GMAT 对照，月节点/边界约定及 STK 同配置轨迹仍待验收；FXM 观测及日 Ap 预测已支持 |
| Drag | Use Approximate Altitude；Use Apparent Sun Position | 已实现：精确 WGS84 高度/椭球径向近似可选，几何/地心 LT+S 视太阳已生效；仍待 STK 同配置数值验收 |
| SRP | Use SRP；Spherical；Cr；Area/Mass | 已实现球形光压、距离平方缩放与独立面积质量比；见[光压契约](solar_radiation_pressure.md) |
| SRP | Shadow Model，含 Dual Cone | 已实现无阴影／圆柱／球面双锥、全照／半影／本影／环食；独立几何和轨迹对照通过，椭球轮廓及专用事件时刻待实现 |
| SRP | Use Boundary Mitigation | 已接入球面接触定位、圆柱单侧光压、双锥高精度边界校正、五种积分器及时间正则化重启、科学插值分段；与 Orekit / Hipparchus 对照事件和轨迹，保留步长减半验证；仍待 STK 实测，不声称复刻内部算法 |
| SRP | Method to Compute Sun，含 Apparent To True CB | 已实现 True、地心视太阳与卫星视太阳，接收型 LT+S、速度时间尺度和历史链校验；遮挡体视位置及 STK 同配置验收待完成 |
| SRP | Atmospheric Altitude；Eclipsing Central Bodies 增删 | 部分：地球／月球独立分配、大气等效半径、多体遮挡并集已实现；可搜索 Available / Assigned 窗口、七颗行星及冥王星的球面配置与十体并集已接入并有 SciPy 对照；DE440s 可直接提供地月／水星／金星本体，火星、外行星及冥王星可导入本体 SPK（逐段过滤、按区间认证、JPLEPHEM 对照）；更多卫星和小天体仍待实现 |
| Additional | Albedo；Thermal；Ck；Area/Mass；Ground Reflection Model | 待实现：各自开关、地表反射/发射率资源、可见地表积分 |
| Additional | Relativistic Accelerations；Satellite Mass | 已接入 IERS 2003 三项相对论、TAI 星历速度、自转轴方向与共享卫星质量；质量用于球形对应面积换算，不覆盖各自显式给定的面积质量比；相对论和球形质量语义分别与 Orekit 对照。见[相对论契约](relativity.md) |
| Additional | Use Plugin；Plugin 选择；Plugin Settings | 待实现：注册、具名设置、生命周期、加速度及偏导契约、可复现配置和错误隔离 |
| Additional | Advanced 子窗口 | 待实现：继续逐项核对官方高级设置；须与力模型版本和数值含义对应，不能放一个空入口 |
| Integrator | Integration 方法 | 部分：RK4、RKF78、RKV 8(9) Efficient、Bulirsch–Stoer、Gauss–Jackson 12 已实现；RKV 已做 SciPy DOP853 / Orekit 数值对照及八阶收敛验证；Bulirsch–Stoer 已做 Hipparchus GBS / Orekit 对照、自动升阶及步长收敛；Gauss–Jackson 已做 Hipparchus GBS / Orekit 对照、固定步长收敛与两种校正验证；RKF 7(8) 已按官方定义改为七阶推进，并通过 Hipparchus / Orekit 对照和七阶收敛验证；STK 同配置验收仍待完成 |
| Integrator | VOP；Predictor Corrector Scheme | 已接入：Gauss–Jackson Full/Pseudo Correction；RKF78 / Bulirsch–Stoer 可选独立的 VOP 通用变量方程，支持固定、自适应及时间正则化，并有 Orekit/Hipparchus 对照；STK 内部实现和同配置实测仍未取得，见 [VOP 积分](vop_integration.md) |
| Integrator | Fixed/Relative Error；Tolerance；Min/Max Step | 部分：独立 Fixed/Relative Error 选择已实现；RK4/GJ 强制固定，RKF/RKV/BS 两模式，固定网格及 BS 十八阶外推已验证。固定/自适应范围已扩至 1e−6–86400 s，自适应缓存不再截短配置上限，并有局部动力学首步估计及长弧独立对照。相对误差仍为混合绝对/相对分量尺度；STK 误差范数、完整合法范围/默认值及实机组合仍待核对，截图仅参考 |
| Integrator | Interpolation 方法；Order；VOP μ | 部分：Lagrange / Hermite / VOP、独立插值 μ、项目自动装配、机动分段、UI / 持久化 / 分析及报告已接入并有独立参考，见[科学插值](orbit_interpolation.md)；VOP 通用变量搬移与 Hipparchus 插值、J2 步长收敛对照已接入，STK 同配置验收仍待完成 |
| Integrator | Allow PosVel Covariance Interpolation | 待实现：协方差传播完成后验证插值、对称性与半正定性 |
| Integrator | Time Regularization；Exponent；Steps Per Orbit | 已接入：Sundman 方程、初始椭圆每圈尺度、五种积分器、物理 Min/Max Step、整数 TAI、天气分段、机动重启及科学插值；C++ / Python / 项目 v40 / UI 共用配置。非椭圆归一化明确拒绝；尚无 STK 同配置实测，见[时间正则化契约](time_regularization.md) |
| Integrator | Report Ephemeris on a Fixed Time Step | 部分：显式工厂及普通项目均支持接受端点或让积分步停在固定 TAI 输出网格，保留天气边界；UI、保存与报告已接入。项目轨道步长当前限于 1–60 整数秒，完整合法范围及短段策略仍待 STK 实测核对 |
| Integrator | Do not propagate below altitude | 已有按力评估停止的参考球面地心高度；STK 比较须核对其高度参考面 |
| Covariance | Compute Covariance；6×6 初始矩阵 | 待实现变分方程与矩阵配置；计算基础已完成中心引力、第三体与三项相对论的解析状态偏导及独立数值对照，见[力状态偏导](force_differential.md)。阻力及球面 SRP 的局部状态／D、K 偏导已提供；卫星视太阳链、事件跳变、单位和正定性政策仍待接入；关闭不影响确定性轨迹 |
| Covariance | Frame；Representation | 待实现：J2000/TOD/LVLH/Frenet，Cartesian 及适用的 FG 表示；完整 Jacobian |
| Covariance | 独立 Gravity Degree / Order | 待接入变分方程；基础已完成：最高 2190 阶解析引力 Hessian、极轴及固定历元潮汐/长期变化系数偏导，通过 Orekit 与高精度解析值对照，见[引力空间偏导](gravity_differential.md)。尚不能配置或传播协方差，后续独立截断不得改变名义轨迹 |
| Covariance | Include Consider Analysis；Use / Value / x,y,z,vx,vy,vz | Drag / SRP 的 D、K 力参数偏导已完成；全局参数敏感度、参数方差及状态交叉协方差仍待实现，OutGasTimeConstant、OutGasMagnitude、ThrustX/Y/Z 还需对应的名义力及参数定义 |
| Covariance | Include Consider Cross Correlation；Row/Column/Value；Add/Remove | 待实现：参数间协方差、联合矩阵校验及输出来源 |

OutGas 与 Thrust 参数必须有对应的名义加速度和偏导定义，不能只存名称或向矩阵添加任意噪声。高级窗口、模型下拉选项的进一步细分须继续依据官方说明和可获得的实际配置核对；核对未完成时保持待实现，不推定已支持。

## 2. 对齐标准

对齐包含四层：模型含义一致、设置能完整往返、桌面与 SDK 使用相同实现、同配置数值结果经过比较。不能以页面上存在同名选项替代任何一层。

- 通用范围覆盖 LEO、MEO、GEO 和高偏心率地球轨道，不能只针对某一高度拟合结果。
- 每个启用的模型都有实际计算、数据依赖、公开参数、失败行为、结果来源与参考用例。
- “支持一种 STK 可选模型”和“复现维护者整组 STK 配置”分开验收。例如实现 NRLMSISE-00，并不意味着截图中选中的 Jacchia–Roberts 已经实现。
- 未实现的选项不允许保存成已启用状态；不以静态大气替代已选经验模型，不以点质量替代缺失球谐文件，不在星历或天气缺失时悄悄关闭摄动。
- 不将积分局部容差描述为长期预报误差。模型截断、输入轨道、空间天气与物性参数的不确定性单独说明。

## 3. 力模型契约

### 3.1 中心引力与潮汐

重力资源保存模型名称、来源、内容哈希、GM、参考半径、归一化方式、系数参考历元、潮汐系统和可用 Degree / Order。对象只保存模型选择及计算截断阶次；不得继续把 EGM96 常数写死在所有数值模型中。初始根数转 Cartesian 也使用所选模型的 GM。

支持 ICGEM `.gfc` 与 STK `.grv` 的明确子集；静态项、长期变化和潮汐块分别解析，遇到影响力模型但尚不支持的记录明确拒绝。按各格式规定处理缺失项，不把 `.grv` 中允许省略的零项误判为损坏，也不忽略文件自身声明的归一化和潮汐约定。[STK 重力文件规范](https://help.agi.com/stk/Content/stk/importfilesGravity.htm)

球谐计算采用可扩展的归一化递推，验证高阶、极点附近和长时间积分；不能把当前未归一化递推中的数组从 21 改大就宣称支持高阶。对象的 Degree / Order 不超过资源范围和经验证的实现范围，额外限制须显示给用户。

时变项按声明的 IERS 版本实现：长期变化、永久潮、完整固体潮、海潮及极潮分别控制，永久潮依据资源的 tide-free / zero-tide 约定处理。完整固体潮还须区分频率相关项、截断规则和幅度阈值；海潮模型文件、潮汐系统和截断阶次进入来源记录。[STK 重力与潮汐](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)、[IERS 2010 第 6 章](https://iers-conventions.obspm.fr/content/chapter6/icc6.pdf)

### 3.2 第三体

首先支持太阳、月球，再接入相同接口的行星与系统质心。数据中心管理 JPL 星历，GM 可来自明确的常数集或用户输入；GM 与位置均记录来源。外行星的系统质心、行星中心和系统 GM 不能混用。

对于地心卫星位置 `r`、地心第三体位置 `rb`，计算

```text
a_b = μ_b [(rb − r) / |rb − r|³ − rb / |rb|³]
```

第二项是地心的加速度，不能遗漏；远距离时使用避免相减损失的等价形式。太阳和月球每次积分阶段都按该阶段历元求值，不按显示帧率更新。

CSPICE 的 `SPICE_J2000` 星历经专用力模型适配器进入计算，保留几何 / 视位置、时标和轴定义。不能直接改标签成 `CartesianState::gcrf`。星历实际目标链、积分全时段及光行时查询余量在准备阶段检查。[STK 第三体设置](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)、[现有 CSPICE 边界](spice.md)

### 3.3 阻力与空间天气

球形阻力以 `Cd` 和 `A_drag / m` 为独立输入：

```text
a_drag = −0.5 ρ Cd (A_drag / m) |v_relative| v_relative
```

`v_relative` 必须相对于大气。使用完整惯性系到地固系状态变换计算共转大气下的相对速度，再将加速度旋回惯性系；不能把惯性速度直接代入。大地高度与“最低地心高度”也不能互换。

目标大气模型至少覆盖截图中的 Jacchia–Roberts 和 STK 可选的 NRLMSISE-00；后者采用包含异常氧贡献的 `gtd7d` 路径。先独立验证密度，再接入轨道积分；后续扩展 Harris–Priester、DTM 等，不用一个近似公式套多个模型名称。高低空模型拼接另行实现连续性与适用域检查。

空间天气有明确的两种模式：手动常数；数据中心文件。输入保留 F10.7、平均 F10.7、Kp / Ap 的区别。文件模式规定观测 / 预报区间、日值或三小时模式、历史窗口、太阳通量采样时刻及插值方法，不能把 Kp 数字直接当成 Ap。NRLMSISE 的日 Ap 与历史 Ap 数组算法分别测试。[STK 阻力与空间天气](https://help.agi.com/stk/Content/hpop/hpopDrag.htm)

### 3.4 光压与阴影

球形光压保存 `Cr`、`A_srp / m`、太阳辐射常数及其距离基准；与阻力面积质量比分开。支持无阴影、圆柱和双锥三种几何，双锥包含全日照、半影、本影与日面未被完全遮住的环食情形。

遮挡体配置包含地球、月球及可选天体。多遮挡体计算日面遮挡的并集，不能直接相乘各自可见比例。地球形状、等效大气高度和太阳半径都是模型配置，必须和对照系统一致。

太阳位置方法区分几何位置、相对卫星的视位置、相对中心天体的视位置。阴影边界单独进行事件检测与步长收敛验证，防止跨越短暂阴影时误差估计没有发现力的不连续。边界修正有自己的算法标识，不能未经验证称为 STK 的内部 mitigation 算法。[STK 光压设置](https://help.agi.com/stk/Content/hpop/hpopSRP.htm)

### 3.5 附加模型与物性

相对论模型明确采用的约定和项；仅实现 Schwarzschild 项时，不能命名为已经包含全部 IERS 相对论项。地球反照、热辐射分别使用反射 / 发射率资源和面积质量比，并验证可见地表积分与日照条件。

球形阻力、球形光压使用各自的面积质量比。质量参数不能暗中覆盖它们；选择面积加质量的输入方式时，转换关系必须唯一。扩展力接口接受只读环境和状态、返回带来源的 SI 加速度，不能在每次求值中修改项目或下载数据。[STK 附加模型](https://help.agi.com/stk/Content/hpop/hpopAdditional.htm)

## 4. 积分与结果输出

力模型、积分算法、方程形式、误差控制、输出采样和插值分别配置。RKF 7(8) 已按官方定义使用七阶推进、八阶嵌入误差估计；采用独立成熟参考和解析收敛验证。误差范数与步长控制器仍是本项目实现，不能据此声称与 STK 逐位相同。

目标包括 RK4、RKF7(8)、RKV8(9)、Bulirsch–Stoer、Gauss–Jackson；VOP 和时间正则化是额外的方程设置，不能作为几个算法的同义名称。每项实现需独立阶数 / 收敛测试，固定步长与自适应控制的有效组合由模型描述规定。Gauss–Jackson 特别验证起步、反向积分与机动后重启。[STK 积分设置](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)

输出时间网格不能决定使用哪套力模型。保持同一历元查询不依赖查询顺序；科学查询使用可验证误差的连续输出或局部积分，显示缓存仍与科学结果分离。支持的 Lagrange / Hermite / VOP 插值要说明阶数、有效区间及是否同时一致地插值速度；不跨脉冲、不跨数据间断插值。

积分诊断包含接受 / 拒绝步数、力评估次数、最小 / 最大实际步长、边界事件和停止原因。停止高度必须说明参考面。资源限制与物理有效域分别报告。

## 5. 协方差

已提供全阶引力 Hessian、第三体／相对论解析状态块，以及[阻力局部状态块和 `D=Cd×A/m` 偏导](drag_differential.md)，均有独立成熟算法或高精度原方程对照。[SRP 固定几何状态／太阳／K 偏导](srp_differential.md)也已完成并经 Orekit、高精度两圆面积与 SciPy 多体积分对照；阻力模型边界及极点、卫星视太阳链、完整 STM、参数传播和协方差装配仍待完成；这些底层接口不代表协方差功能已启用。

协方差是独立能力，不能只增加一个矩阵编辑框。第一层为 6×6 初始状态协方差与状态转移矩阵：

```text
Φ_dot = A(t) Φ,     Φ(t0) = I
P(t) = Φ(t) P0 Φ(t)ᵀ              （无过程噪声）
```

`A` 来自当前启用的力模型对位置、速度的偏导；阻力、第三体和光压不能在轨迹中开启却从变分方程中消失。若对协方差采用单独的重力截断阶次，要显式保存并报告。

初始矩阵检查有限性、对称性、单位、半正定性和所选算法的退化矩阵政策。输出坐标转换采用完整 6×6 状态变换 Jacobian，含旋转率；不能只对位置块做旋转。LVLH 与 Frenet 使用明确的轴定义。

第二层再接 `D=Cd×DragArea/Mass` / `K=Cr×SRPArea/Mass` 等参数敏感度、参数协方差、状态与参数交叉协方差、consider analysis 及可选过程噪声，报告区分这些来源。轨迹、状态协方差和敏感度共享积分历元与事件处理。脉冲处需明确状态跳变 Jacobian 和机动执行误差，未定义误差模型时不补造噪声。[STK 协方差说明](https://help.agi.com/stk/Content/hpop/hpop-covariance.htm)

## 6. 数据与模块改造

### 配置、资源、计算分开

- `HpopDefinition`：不可变、可序列化的力模型 / 积分 / 协方差配置，没有本机文件路径和 Provider 对象。
- `ForceEnvironment`：准备阶段解析出的只读时间、EOP、重力、星历、空间天气与潮汐资源，明确共同有效区间及实际模型版本。
- `ForceEvaluator`：统一求总加速度、分项加速度和需要的偏导；传播、报告、对照工具及机动滑行复用此入口。
- `NumericalPropagator`：推进状态及可选变分方程，管理事件、缓存、诊断和取消，不承担目录查找及联网。

上述名称是重构中的职责名称，不是 0.2.1 已存在的 API。Core 继续只依赖标准库和抽象接口；外部大气算法、资源导入与 CSPICE 适配进入 Provider / Runtime。不能把更多模型直接堆入当前 `Hpop::derivative()`。

### 数据中心

左侧按数据类型管理：地球定向、UTC / 闰秒、重力场、行星星历、空间天气、潮汐 / 地表辐射。右侧显示来源、下载时间、实际覆盖、状态及更新操作。静态重力文件没有人为的“过期日期”；时变系数的参考历元与适用范围另行展示。

按启用模型计算所需资源与时间范围，覆盖从初始状态到分析时段的完整积分路径，并包含天气历史与光行时查询余量。下载、导入、内容校验和激活保持原子发布；计算期间资源冻结。

项目记录实际使用的数据角色、内容哈希、解释配置及算法版本。新版本数据与已记录数据不一致时沿用已有确认流程；拒绝则停止打开或重新计算。覆盖不足时说明缺少什么数据、所需时段与处理入口，不悄悄替换成低精度模型。EOP 的有限外推政策不能自动延伸到星历或空间天气。

### API 与持久化

Python 采用明确的模型值对象与关键字参数，卫星继续通过 `project.objects.satellite(...)` 创建。设计形态如下，**不是当前可执行示例**：

```python
sat = project.objects.satellite("MySat", orbit=of.orbits.Hpop(
    initial=initial,
    gravity=of.forces.Gravity("EGM2008", degree=21, order=21),
    third_bodies=("sun", "moon"),
    drag=of.forces.Drag(
        model="jacchia_roberts", cd=2.2, area_mass_ratio=0.02,
        weather=of.weather.Manual(f107=150, f107_average=150, kp=3),
    ),
    srp=of.forces.SRP(cr=1.0, area_mass_ratio=0.02, shadow="dual_cone"),
))
analysis = project.prepare(data=data)
```

默认值按经过验证的模型配置定义并写入文件，不根据电脑上恰好存在的数据自动改变物理模型。编辑与构造使用相同校验；禁用模型的草稿可在界面保留，实际计算身份只包含生效配置。

C++ 共享同一份具名强类型定义，Python 不要求创建 `Update` 包装器；两者不暴露插件裸指针作为项目持久化字段。改动项目格式时提升版本，替换现有读写与固定样例，按仓库要求拒绝旧格式，不追加历史格式兼容分支。

## 7. 桌面与报告

配置窗口使用清晰的导航分组：概览、重力、阻力、光压、附加模型、积分、协方差。内容参考 STK 的职责划分，视觉继续采用 OrbitForge 的深蓝灰 / 冰蓝主题、克制的樱粉点缀与响应式布局。

桌面设置以左侧导航分“力模型”（引力、阻力、太阳辐射压、其他）和“数值”（积分器、协方差），每项显示当前取值摘要；窄窗口改用页签。中心天体引力与第三体位于引力页，最低停止高度位于积分器页。未实现的模型与协方差页显示能力状态，不提供伪生效开关；这次导航结构调整不代表对应数值功能已完成。

概览给出已启用模型、缺失资源和计算范围；详细页面只呈现相关参数。第三体使用可搜索表格，遮挡体使用统一对象 / 天体选择窗口。选择模型时同步单位、有效范围、需要的天气参数及是否支持低空拼接；不能把所有参数无差别地挤在一个表单中。

积分与协方差独立成页。矩阵编辑支持下三角输入、对称显示、粘贴、单位提示与错误定位。取消不提交，关闭重开不丢弃已经保存的配置草稿，宽窄切换不重建编辑器。应用前显示缺失依赖；后台传播可取消。

报告新增总加速度 / 分项摄动、密度及空间天气、光照比例 / 阴影区间、积分诊断、协方差分量与不确定度曲线。它们来自实际传播环境和结果，不能从可视化轨迹反算后冒充积分所用数据。

## 8. 验收与推进顺序

| 阶段 | 完整交付范围 | 放行条件 |
| --- | --- | --- |
| A：配置和资源基础 | 力模型 / 积分 / 协方差配置解耦；通用重力资源；EGM2008 / EGM96；星历接入快照及项目引用 | 资源损坏 / 不支持项 / 覆盖不足均明确报错；配置跨 C++、Python、项目文件一致 |
| B：常用完整力环境 | 日月引力、Jacchia–Roberts / NRLMSISE-00、天气输入、球形光压与地月阴影；桌面与报告贯通 | LEO、MEO、GEO、HEO 的分项与组合对照；输入变更后全部调用方一致生效 |
| C：时变与附加模型 | 长期重力变化、永久 / 完整固体潮、海潮、相对论、反照 / 热辐射 | 固定历元的分项参考、潮汐系统和能量 / 单位 / 方向检查通过 |
| D：积分高级能力 | 其余积分器、VOP、时间正则化、科学插值和事件诊断 | 阶数与容差收敛、正反向、偏心轨道、阴影边界、机动重启及性能基准通过 |
| E：不确定度 | STM、状态协方差、坐标变换、参数敏感度和 consider analysis | 有限差分参考、协方差性质、跨坐标系、机动跳变与独立结果比较通过 |

每个阶段都以可用的计算、数据、API、持久化、桌面和验证闭环交付，不先放一整套可点击但不生效的设置。阶段 A 的静态重力与 DE440s 已完成；桌面分页完成，内部配置进一步解耦仍待推进。阶段 B 已实现太阳系第三体引力和球形光压／球面地月遮挡；球形阻力、JR/NRLMSISE-00、手动天气、近似高度、阻力地心视太阳已实现；CelesTrak 天气文件及历史取样已接入；光压三种太阳位置已接入；三小时自然样条已接入（节点/端点约定待 STK 验收）；FXM 观测与日 Ap 预测、1 AU 通量改正已接入（改正节点约定待 STK 验收）；保持窗口均值的插值已接入（边界/限制约定待 STK 验收）；CelesTrak 月预报和 CSSI Predicts 名义预报已接入（节点/平均构造待 STK 验收）；遮挡体视位置与阴影边界高级处理仍待实现。阶段 B 是“常用完整力环境”的最低门槛；完成 B 仍不代表潮汐、全部积分器和协方差已与 STK 全面对齐。完整验收以本文件逐项清单全部通过为准。

### 数值比较

1. 固定 STK 版本、完整场景设置和数据原始字节；同时记录 GM、参考半径、太阳常数、潮汐系统、天气和截断阶次。不能只记录“都用了 HPOP”。
2. 统一惯性坐标系。STK 的 J2000 与 ICRF 不相同；本项目 GCRF 对照地心 ICRF，若参考输出为 J2000，显式转换。时间、EOP 插值及章动 / 极移模型同步记录。[STK 参考系](https://help.agi.com/stk/content/stk/referenceframesvgt.htm)
3. 先比较单点加速度和环境输入，再逐个开启第三体、阻力、光压、潮汐等，最后比较组合轨道。给出每轴、范数及 RTN 的最大值 / RMS / 终点差，不能只比较终点距离。
4. 使用低轨、导航高度、同步轨道及高偏心率案例；包含 ±时间、跨 UTC 日界、三小时天气边界、日月食、近极轨、覆盖端点、随机顺序与并发查询。
5. 将数值积分误差、物理模型差异和数据差异分开。用更严格容差 / 减半步长确认收敛，再依据模型和参考数据精度确定每类测试容限；不以调宽阈值消除差异。
6. STK 直接输出尚未取得前，继续使用独立 Orekit / 模型官方参考做回归，并明确标记“尚未完成 STK 数值验收”。截图只能证明参数和界面含义，不能生成参考轨迹或精度结论。

实际运行证据进入 `out/validation/`，可复现输入和协议进入 `tests/reference/` 与 `docs/validation/`；不提交维护者的原始截图、个人场景或程序安装文件。
