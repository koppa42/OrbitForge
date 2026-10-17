# 科学轨道星历插值

`core::make_tabulated_orbit` 与 `runtime::make_tabulated_orbit_session` 接收显式的 `OrbitEphemerisArc` 数组，拥有不可变节点副本。桌面显示轨迹不进入这个接口；它是科学查询的计算基础。`propagate_hpop_ephemeris` 已能生成真实 HPOP 节点，`make_hpop_ephemeris_session` 将其接到同一插值实现，详见下文。普通项目、任务 HPOP 滑行、分析与报告已自动使用有界科学星历；积分器页配置插值方法、阶数、VOP μ 与固定输出。直接积分工厂保留为数值内核和对照入口。

## 数学语义

按 [STK Integrator](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm) 的定义，`lagrange` 对位置和速度各分量分别插值；输出速度一般不等于位置多项式的导数。`hermite` 同时约束节点位置与速度，输出同一个位置多项式及其对 TAI 秒的导数。调用者必须提供该参考系中相对于 TAI 秒的坐标导数，不能将 TDB 速度或不同坐标系混入。

[STK Connect](https://help.agi.com/stk/SubSystems/connectCmds/Content/cmd_HPOPIntegrator.htm) 的阶数范围对应多项式次数：Lagrange / VOP 为 0–29，需要 `order+1` 点；Hermite 为奇数 3–29，需要 `(order+1)/2` 点。`OrbitInterpolationOptions` 默认 Hermite 7 是本接口的选择，不声称是 STK 默认值。VOP 使用独立、有限且正的 `vop_mu`（m³/s²），默认 `3.986004415e14`；此默认是 OrbitForge 的选择，不覆盖底场 GM。VOP 插值独立于积分方程，当前积分仍采用 Cowell。

查询先保留左右包围节点，再按距离补齐连续窗口，距离相同优先较早节点；这样不等间隔采样也不会在局部窗口外求值。窗口在同一段内向端点移动，不减少阶数。Lagrange 0 阶按左节点保持，并将节点时刻标为间断。精确命中任一整数纳秒节点时直接返回原状态，包括输入速度。这些窗口、零阶及端点约定是 OrbitForge 的明确政策，仍需 STK 同配置实测核对。

计算先求局部 TAI 秒差，以窗口跨度归一化，避免大绝对历元损失相邻纳秒。Lagrange 用第二重心公式、权重归一化及补偿求和，并减去邻近节点的状态常数。Hermite 用合流 Newton 差商，先扣除邻近节点的直线运动，再用同一嵌套多项式同时求值及导数。无法分辨的浮点节点、病态重心权重或非有限结果明确报错。高阶并不保证高精度；调用者仍须用减半采样间隔和独立轨迹检验误差。

## VOP 插值

选择 `OrbitInterpolationMethod::vop` 时，以查询历元为共同参考，先将每个窗口节点 `X_i` 通过通用变量二体映射 `Φ(t−t_i, X_i; μ)` 搬移至查询时刻，再用原节点时刻的 Lagrange 权重分别插值六个 Cartesian 参数：`X(t)=Σ L_i(t) Φ(t−t_i, X_i; μ)`。正向、反向、椭圆、抛物和双曲映射使用半径归一化的 Stumpff 方程及有界求根；椭圆多圈时间按周期约减。它不查询力模型或外部数据，也不改变积分器的方程形式。

该配方满足公开 VOP 参数语义，并有独立 Orekit / Hipparchus 对照；STK 未公开此插值器的全部实现细节，不能据此声称逐位复现 STK。窗口及端点规则与 Lagrange 相同；零阶将左节点按二体运动传播到查询时刻，不保持原位置。精确节点直接返回输入。仅接受地心 GCRF / SPICE J2000 惯性轴下非零半径、数值可分辨的非径向轨道；拒绝 ITRF、TEME、径向碰撞和数值不可分辨的状态。速度作为搬移后的独立参数插值，不承诺等于最终位置表达式的导数。

未选 VOP 时 μ 是有效草稿，保存但不参与计算和身份。选择 VOP 后，μ 与 `vop-universal-transport-v1` 算法标识共同进入来源及分析/报告身份。两个节点之间的变化越偏离二体运动，越需要缩短节点间隔；阶数提高或选用 VOP 本身均不构成误差保证。

## 时间段与所有权

- SI 位置/速度，`Epoch` 为 int64 TAI 纳秒；全部节点必须为相同的已知原点、参考系和有限状态。
- 每段节点严格递增，所有段按开始时间严格排序；段可以共享端点，但不可重叠。共享端点采用后一段状态，表达机动后的右连续状态。
- 段间缺口和覆盖外查询抛出 `PropagationError`，不外推、不跨段补点；调用者负责在机动、非光滑力或数据边界分段。节点不足以满足指定阶数时，构造抛出 `invalid_argument`，即使查询只准备命中现有节点也不自动降阶。
- `discontinuities()` 返回段连接/缺口边界；零阶额外返回各跳变节点。当前接口不从任意节点猜测机动或生成 `ManeuverEvent`。
- 创建时校验全部输入，取消或失败不发布对象；查询无可变缓存，随机顺序及并发读取互不影响。复制进入接口的数组、原始 Session 或选项之后修改，均不影响已发布对象。结果保有查询历元和来源信息。

来源字符串记录算法版本、方法、阶数与活动 VOP μ，调用者提供对象 ID 和输入修订。它不是外部节点文件的内容哈希，也不会把一份任意表接入项目的数据引用。HPOP 显式工厂已记录节点输出、分段、插值及区间身份；项目身份、持久化、UI 与报告采用同一配置，首次生成通过可取消的查询路径执行。

## C++ 与 Python

C++ 经 `orbitforge/orbitforge.hpp` 使用 `OrbitInterpolationMethod`、`OrbitInterpolationOptions`、`OrbitEphemerisArc`、`make_tabulated_orbit_session`，只依赖 Runtime，不需要 CSPICE、ERFA 或 Qt。Core 工厂只依赖标准库。

```python
import orbitforge as of

options = of.OrbitInterpolationOptions()
options.method = of.OrbitInterpolationMethod.HERMITE
options.order = 3
nodes = [
    of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                      [0, 0, 0], [0, 0, 0]),
    of.CartesianState(of.Epoch(2_000_000_000), of.OriginId.EARTH, of.FrameId.GCRF,
                      [8, 0, 0], [12, 0, 0]),
]
session = of.make_tabulated_orbit_session([nodes], options, "cubic", 1)
state = session.sample([of.Epoch(1_000_000_000)]).states[0]
assert state.position_m == [1, 0, 0]
assert state.velocity_mps == [3, 0, 0]
```

示例是验证用三次多项式，不是物理地球轨道。数值参考、输入散列和误差收敛见[插值验证协议](../validation/orbit_interpolation_protocol.md)。

## HPOP 接受节点与固定输出

`core::propagate_hpop_ephemeris(initial, end, options, environment, reporting_step_nanoseconds, checkpoint)` 在初始状态与结束历元之间生成一个连续滑行段，可正向或反向。输出拥有节点，按时间递增排序；节点来自五种积分器接受的步骤，拒绝步骤不会进入结果，Gauss–Jackson 启动器的内部子步不作为独立输出节点。

`reporting_step_nanoseconds=0` 保存接受端点。正值表示以初始历元为起点、精确整数 TAI 纳秒的固定输出网格；积分步会在输出时刻截短，始末点始终保留。两者都在已有天气更新边界分段，边界状态由前后两段共享；固定输出模式额外保留不在输出网格上的天气边界。边界内仍采用原有单侧力求值，包含正闰秒和 JR 天气滞后。

这条路径整段连续推进，不使用直接查询的 300 s / Max Step 缓存块。自适应方法在固定输出点保留下一步控制估计；固定方法仍以初始积分网格为准，在报告/天气边界截短。Gauss–Jackson 遇到不完整步时使用已验证的启动器并清空历史，然后从原网格恢复；输出间隔与固定积分步不相容时可能频繁启动，这是当前明确的策略，尚未取得 STK 内部算法的实测对照。

`runtime::make_hpop_ephemeris_session` 先生成完整滑行段，再构造不可变插值 Session。它要求显式的插值方法与阶数；每个天气段都必须有足够节点，否则构造失败。它不自动降阶、延长传播或在查询时切回积分。此工厂是一次有界、可取消的传播操作，失败不返回部分结果；已生成 Session 的查询没有后续力模型求值。来源记录初始/结束 TAI 纳秒、固定输出间隔、方法、阶数及实际力环境身份。

两种入口均限定距初始历元最多 366 天、整次生成最多 200000 次积分尝试，输出间隔为 0–86400 s。输出网格不能在相对 double 秒中分辨时明确报错，不能复制前一状态冒充新积分结果。所有积分阶段均限于请求的滑行区间；结束历元保留精确整数纳秒，长区间浮点取整不会向外扩展力模型覆盖。不为补齐插值点向外查询数据。这两个显式接口仅接受单段 HPOP；任务序列通过下述有界工厂自动装配各滑行段。调用者手动接续机动时，应在机动历元结束该段，用机动后状态创建下一段。

Python 使用同名入口，`input` 为低层 `OrbitInput`，必须选择 HPOP；资源通过显式 `DataSnapshot` 捕获，`cancelled` 为返回 bool 的回调。传播范围由 `input.initial.epoch` 与显式 `end` 决定，不使用 `input.first`、`input.last` 或 `input.step_seconds`；输出间隔通过上述纳秒参数单独指定。这是低层显式输出入口；高层 `Project.prepare()` 按对象保存的设置装配，见后文：

```python
import math
import orbitforge as of

orbit = of.OrbitInput()
orbit.propagator = of.OrbitPropagator.HPOP
orbit.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                  [7e6, 0, 0], [0, math.sqrt(3.986004415e14 / 7e6), 0])
force = of.HpopOptions()
force.gravity_degree = force.gravity_order = 0
orbit.hpop = force
interp = of.OrbitInterpolationOptions()
interp.method = of.OrbitInterpolationMethod.HERMITE
interp.order = 7
nodes = of.propagate_hpop_ephemeris(orbit, of.Epoch(600_000_000_000),
                                   reporting_step_nanoseconds=30_000_000_000)
session = of.make_hpop_ephemeris_session(orbit, of.Epoch(600_000_000_000), interp,
                                        reporting_step_nanoseconds=30_000_000_000)
state = session.sample([of.Epoch(17_123_456_789)]).states[0]
```

这是一段显式的点质量测试轨道。非中心引力、第三体、阻力、光压等照常要求对应的数据快照，不隐式下载或查找本机数据。输入与资源在调用时捕获；后续修改输入、取消另一次生成或销毁原数组均不影响已生成 Session。

## 项目、机动与桌面

`core::make_hpop_interpolated(initial, first, last, options, environment, reporting_step_nanoseconds)` 与 Runtime 同名 Session 工厂只允许在 `[first,last]` 内查询。构造检查从初始历元至整个区间的资源覆盖和参数；首次状态或间断查询才生成完整前向/反向滑行段，并发布不可变节点。取消与失败不发布半成品，之后可重试；发布后查询不再求力。可取消入口包括 `state_at(epoch, checkpoint)`、`discontinuities(checkpoint)`、报告与桌面后台准备；Python 单点 `Analysis.state()` 仍是同步接口。

接受节点模式在短滑行或短天气段内增加实际积分终点，以满足指定阶数所需点数。触发条件为该段长度小于 `(所需点数−1) × 有效最大步长`；内部支持时刻按整数纳秒等分，保留初始固定积分网格及真实接受端点。所有点位于该段内，不延长资源覆盖、不降阶。过短而不能表示足够独立纳秒时刻的段明确失败。这个支持点政策是 OrbitForge 的实现，尚未取得 STK 实机对照。

固定输出模式严格使用显式网格和天气边界，不补额外支持点。如果任一天气段或机动滑行段的点数不足以满足所选阶数，生成失败；用户需减小轨道步长或选择合法的较低阶数。零时长且等于初始历元的段返回精确初态，允许同一时刻连续机动和最终机动，不构造假节点。

任务工厂将每次脉冲的精确历元传给 `CoastFactory(initial, end, checkpoint)`；下一段从机动后状态重新生成。项目只计算到自己的结束历元，结束时的全部同历元脉冲仍按顺序执行。天气边界、插值跳变和机动历元合并到间断列表，插值窗口不跨这些边界。任务计划在项目结束之后的部分仍保存并验证结构，但不向那部分请求力模型资源。

`HpopOptions.interpolation` 默认 Hermite 7，`report_ephemeris_on_fixed_step` 默认关闭。高层 Python 使用 `Hpop(interpolation="lagrange", interpolation_order=7, report_ephemeris_on_fixed_step=True)`；开启固定输出时，`Orbit.step_seconds`（目前 1–60 整数秒）决定星历节点网格，可通过 `dataclasses.replace(orbit, step_seconds=5)` 设置。报告和 `analysis.states()` 的查询步长仍各自独立。显式低层工厂以自己的输出间隔参数为准。

桌面积分器页支持三种方法及 VOP 独立 μ、合法阶数和固定输出开关，方法切换保留各自未应用的阶数草稿；确认时统一校验，取消不改输入。轨道页名称为“轨道步长”。插值方法/阶数、μ 草稿和输出开关随项目 v40 / OBJS31 保存；固定输出启用时轨道步长同时进入分析和报告身份，关闭时仅是显示设置。旧分析持有原配置与资源，编辑不改变已有查询结果。VOP 积分由独立开关控制，见 [VOP 积分](vop_integration.md)；协方差插值与 STK 同配置数值验收仍未完成。

高层 Python 的 VOP 配置示例（`initial` 为显式地心 GCRF 初态）：

```python
orbit = of.orbits.Hpop(initial=initial, gravity_degree=0,
                      interpolation="vop", interpolation_order=7,
                      interpolation_mu=3.986004415e14)
```

低层 `OrbitInterpolationOptions` 对应 `method=OrbitInterpolationMethod.VOP`、`order` 和 `vop_mu`，与 HPOP 的 `gravity_mu` 独立。
