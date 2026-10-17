# 任务序列与脉冲机动

卫星传播器新增“任务序列 · 机动规划”。它采用 STK Astrogator 的初始状态、传播段和机动段的顺序组织方式：每一段继承前一段的末状态。当前实现用于手动设计简单变轨，不包含自动目标求解。

## 模型与边界

| 内容 | OrbitForge 当前实现 |
| --- | --- |
| 初始状态 | 卫星的地心 GCRF 瞬时位置/速度；可从经典根数转换 |
| 滑行 | 正的 TAI 时长；整条任务使用二体或 HPOP |
| 机动 | 瞬时脉冲，保持位置不变，将指定 Δv 加到速度上 |
| 方向 | GCRF、VNC、LVLH，局部轴由每次点火前的状态确定 |
| 停止条件 | 指定滑行时长 |
| 输出 | 既有状态、根数、报告、Access/AER 和显示；另有机动前后状态与累计 Δv 查询 |
| 尚未实现 | 有限推力、推力方向随时间变化、质量/燃料、轨道事件停止条件、目标求解器、优化器、分支和循环 |

参考 STK 的[传播段](https://help.agi.com/stk/Content/gator/mc-prop.htm)、[脉冲机动段](https://help.agi.com/stk/Content/gator/mc-impmaneuvers.htm)与[坐标系定义](https://help.agi.com/stk/Content/gator/eq-coordsys.htm)。相同的组织方式不表示已实现全部 Astrogator 功能或达到其完整力模型精度。

VNC 的 X 是速度单位向量、Y 是轨道法向 `unit(r × v)`、Z 是 `X × Y`。LVLH 沿用 STK 定义：X 为径向向外、Z 为轨道法向、Y 为 `Z × X`；它不同于 VVLH。速度为零、径向与速度平行或近乎平行时，局部机动轴无法定义，计算报错。

任务包含 1–256 段，至少一段正时长滑行，总时长不超过 366 天，历元加法不得溢出。允许初始或最终历元点火，也允许同一时刻连续脉冲；连续脉冲依列表顺序执行，每次重新计算局部轴。累计 Δv 是各脉冲大小之和，不是向量和，不推算燃料。

任务只覆盖 `[初始历元, 初始历元 + 滑行总时长]`。场景必须在此范围内；扩大场景后需显式延长计划。区间内允许乱序和重复查询，点火时刻返回**全部同刻脉冲执行后的状态**。查询机动记录可取得每次点火的前后状态；场景之前的机动虽不出现在返回列表中，但仍参与初态传播和累计 Δv。

二体使用现有 CSPICE Provider 和 IERS 2003 μ。HPOP 使用 EGM96 μ、现有球谐引力及积分选项；非中心引力必须显式提供覆盖初始历元至场景结束的 EOP，GCRF 输出也不豁免。HPOP 力模型的限制见[轨道预报器](propagators.md)。每次脉冲重新建立滑行初值；连续滑行段不人为重置积分器。

## 桌面

在卫星属性中选择“任务序列 · 机动规划”，点击“规划…”。左侧管理段列表，支持添加、删除、上移和下移；右侧编辑名称、时长、方向和三轴 Δv，并显示本次脉冲合量与坐标轴含义。每段显示滑行起止或点火时刻，`T+` 相对卫星初始历元，按 TAI 时长累计，保留纳秒；调整时长或顺序后同步更新后续时间。顶部选择共同的滑行模型，HPOP 可进入模型设置。底部显示总时长、段数、总 Δv，以及计划是否覆盖场景；不足时显示缺少的时长。

新计划默认包含覆盖当前初始历元至场景结束的一段滑行。插入机动后，可将点火前后时长分配给两段滑行。参数使用已有单位控件；宽窗口为左右布局，窄窗口用“任务序列 / 段参数”页签切换，参数区独立滚动，保存操作始终可达。调整尺寸与页面切换保留当前段、单位和未完成输入。长名称省略显示，可悬停查看完整信息。

输入错误会阻止切换到其他段或保存，并说明预览仍使用上次有效参数；修正后自动恢复，删除当前段可丢弃该段草稿。删除最后一段后显示空状态，必须重新添加滑行才能保存。列表中 `Alt+↑` / `Alt+↓` 调整顺序，`Delete` 删除当前段，`Enter` 打开段参数；这些编辑操作不会保存整个计划。取消窗口不提交计划；“保存计划”（`Ctrl+Enter`）只更新卫星属性草稿，轨道页的“应用”才在后台传播并发布场景。

轨迹缓存加入机动时刻及其前一纳秒，以分别保存速度跳变两侧的切线；不存在可表示的查询时刻落在这两个相邻纳秒之间。既有 1 m 显示插值校验继续执行，显示采样不参与分析结果。报告按所选时间网格采样，不自动插入点火行；精确机动记录使用 `analysis.maneuvers()`。

## Python

```python
import orbitforge as of
from orbitforge import units as u
from orbitforge.missions import Coast, Impulse

project = of.Project("变轨", of.Epoch(0), of.Epoch(7_200_000_000_000))
satellite = project.objects.satellite("Transfer", orbit=of.orbits.Mission(
    initial=of.orbits.Keplerian(project.first, u.km(7000), 0, u.deg(28)),
    segments=[
        Coast(u.s(600), name="等待点火"),
        Impulse((120, 0, 0), frame="vnc", name="升轨"),  # m/s
        Coast(u.s(6600)),
    ],
))
analysis = project.prepare()
trajectory = analysis.states(satellite, step=u.s(60))
burns = analysis.maneuvers(satellite)
for burn in burns.events:
    print(burn.name, burn.before.epoch.tai_nanoseconds,
          burn.delta_v_gcrf_mps, burn.cumulative_delta_v_mps)
```

`segments` 支持列表或元组，配置捕获为不可变的 `missions.Plan`；脉冲输入明确使用 `delta_v_mps`，默认 VNC。时长用 `u.s()` / `u.ns()` 精确表示。修改计划可用 `dataclasses.replace`，再将新 `Orbit` 赋回 `satellite.orbit`；旧分析继续持有旧计划与已捕获数据。

HPOP 滑行使用 `propagation="hpop", hpop=of.orbits.HpopSettings(...)`。点质量可设 `gravity_degree=0, gravity_order=0`，无需 EOP；默认 8×8 则需 `project.prepare(data=...)`。完整的两脉冲霍曼变轨见[可运行示例](../../examples/python/mission.py)。

## C++

```cpp
#include <orbitforge/orbitforge.hpp>
namespace of = orbitforge;
using namespace std::chrono_literals;

of::Project project{"Transfer", of::Epoch{0}, of::Epoch{7'200'000'000'000}};
of::OrbitInput orbit;
orbit.propagator = of::OrbitPropagator::mission;
orbit.mission = {of::MissionCoastModel::two_body, {
    of::MissionSegment::coast(600s, "Coast"),
    of::MissionSegment::impulse({120, 0, 0}, of::ManeuverFrame::vnc, "Raise"),
    of::MissionSegment::coast(6600s)
}};
auto satellite = project.objects().create(
    of::objects::Satellite{.name = "Transfer", .orbit = orbit});
auto analysis = project.prepare();
auto burns = analysis.maneuvers(satellite);
auto states = analysis.states(satellite, 60s, of::FrameId::gcrf);
```

公共段类型位于 Core；`runtime::make_mission_session` 装配二体或 HPOP，桌面与两种 SDK 共用。`ManeuverSeries` 拥有结果、对象/修订/算法来源及输入和数据哈希；非任务序列卫星返回空记录，地面站和传感器不接受机动查询。取消回调可中止传播，失败不返回部分结果；已完成弧段可缓存，重试保持一致。

`.orbitforge` v40 保存初态、滑行模型、全部有序段及活动 HPOP 配置，只读写当前格式。机动名称属于显示元数据，不改变计算输入哈希；时长、顺序、方向和 Δv 均影响哈希。固定二进制样本由独立 `struct.pack` 构造，精确校验纳秒字段。验证范围与容限见[机动验证协议](../validation/mission_protocol.md)。

HPOP 滑行使用[科学插值星历](orbit_interpolation.md)：每次脉冲处结束旧段，以机动后状态开始新段，并限制在项目结束时刻。Core `CoastFactory` 接收精确结束历元及取消检查点；`make_mission(..., stop)` 可指定计划前缀。结束历元上的全部脉冲仍执行，后续计划不传播。`discontinuities(checkpoint)` 合并天气、插值和机动边界，其首次生成可取消。
