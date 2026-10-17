# OrbitForge 对象 API 与数据驱动设计

**状态：分阶段实施中。** 公共对象管理、事务、变更通知与 Python 易用接口见[对象编辑 API](object_editing.md)；分析快照、准备确认计划及目录按时段选源见[分析 API](analysis_api.md)。本文仍定义完整目标；C++ 顶层门面已实现，见 [C++ 项目 API](cpp_api.md)；桌面报告已接入固定分析，工作台对象应用和图形准备尚未整体迁入统一文档。强类型轨道配置和描述表等未完成部分不能直接用于当前 0.2.1 包。

重构以 `project.objects` 为统一对象入口，使 Python、C++ SDK 和桌面共享对象模型、校验、事务与变更通知。计算通过固定输入的 `Analysis` 显式执行；界面根据对象能力和变更依赖更新，避免把纯显示修改当成轨道输入变化。

Python 以脚本易用性为优先，提供可补全的创建方法、关键字参数、带类型对象和属性访问。普通脚本只需掌握项目、对象、分析和结果；事务、数据确认计划和报告模板按需求使用。C++ 使用配置值、对象引用和类型化更新结构。两端的单位、校验、事务和结果语义一致，公开签名遵循各自语言习惯。

当前实现及物理约束仍以[模块契约](p0_contracts.md)、[项目文件](project_files.md)、[数据中心](data_center.md)、[数据引用](scenario_data_lock.md)、[传感器与可见性](access_sensors.md)和[报告](reports.md)为准。本设计定义后续重构目标，不将尚未实现的能力列为现有功能。

## 目标与范围

- 用统一入口管理卫星、地面站和传感器，保留对象类型、单位、参考系和失败行为的明确约束。
- Python 常用操作直接接受关键字参数和对象属性，自动补全可发现参数，不要求用户手工构造更新结构。
- 将候选修改、校验和提交逻辑移入公共应用层，Python 绑定和桌面不各自实现一套业务规则。
- 分开项目输入、运行时分析、计算结果、显示缓存与工作区状态。
- 用描述表驱动类型列表、常规属性编辑、对象筛选和报告字段；数值算法仍由强类型 C++ 实现。
- 保留现有数值模型，按可独立验证的阶段重构；开发阶段只维护当前文件格式。

第一阶段不引入通用撤销栈、动态插件语言、通用分析任务系统或新的轨道算法。文件格式按持久化需求演进，不要求改用 JSON，也不因 API 改名而替换现有容器。

## Python 易用性约定

| 使用环节 | 约定 |
| --- | --- |
| 创建对象 | 在 `project.objects` 下直接选择 `satellite()`、`ground_station()`、`sensor()`，签名列出参数并返回对应类型对象 |
| 日常编辑 | 读取属性、单属性赋值、多属性 `update()`；一次调用的修改保持原子性 |
| 批量编辑 | `with project.edit():` 内继续使用原对象和 `project.objects`，成功退出时一起提交 |
| 查找对象 | 按 ID 用 `objects[id]`，允许缺失时用 `objects.get(id)`；名称搜索返回所有匹配项 |
| 计算 | 准备一次 `Analysis` 后执行多次查询；属性访问不触发传播、下载或文件写入 |
| 使用结果 | 直接读取有名称的数值列、遍历区间、导出 CSV；需要时转换 NumPy 或 pandas |
| 调试与学习 | `help()`、签名、类型提示与可读 `repr` 反映真实能力；错误指出具体对象和字段 |

每个操作有一个日常推荐入口。通用配置创建用于导入或程序化工具，报告模板用于复用格式；入门示例不要求先掌握这些抽象。物理量继续显式带单位，复杂配置保留其领域含义，默认值在签名和文档中说明。

## 公开入口

| 入口 | 职责 |
| --- | --- |
| `of.Project` | 项目创建、打开、事务、分析准备与保存的公共门面 |
| `of.objects` | 卫星、地面站、传感器的类型与配置值 |
| `project.objects` | 对象创建、查询、修改、复制和删除 |
| `project.edit()` | 将一组对象或项目设置修改组成一次事务；Python 在上下文中继续使用原对象 |
| `of.DataCatalog` | 本机当前数据文件、导入和更新的管理入口 |
| `DataSnapshot` | 固定本次可使用的数据资源及策略 |
| `Analysis` | 在固定项目输入与数据上执行数值查询 |
| `of.reports` | 查询配置、报告模板和结果呈现 |

常用接口不要求用户手工组装 `Scenario`、`Session` 和 Provider。独立传播器及其他低层计算接口仍可按需使用，无界面调用不引入桌面依赖。

## 对象管理契约

### 配置与引用

`of.objects.Satellite(...)` 等构造调用产生配置值；`project.objects.create(definition)` 将配置加入项目。C++ 返回带类型的 `ObjectRef`，Python 返回包装该引用与所属项目的带类型对象句柄。修改调用方原有的配置值不会回写项目。

Python 的日常创建入口为 `project.objects.satellite(name, *, orbit, ...)`、`ground_station(name, *, location, ...)` 和 `sensor(name, *, parent, field_of_view, ...)`。每个方法给出固定签名、参数提示和返回类型；名称可以按位置传入，物理配置使用关键字参数。门面转换成相应配置值并调用公共创建命令，不重复实现业务规则。

导入和通用工具可以传入 `create(definition)`。Python 不再把 `create(Type, **fields)` 作为另一套公开重载，避免普通用户需要先传类型才能发现参数；已有配置实例也不再接受额外覆盖参数。

第一阶段提供 `Satellite`、`GroundStation` 和 `Sensor`。轨道模型、视场和指向方式是对象的配置，暂不作为独立场景节点。传感器只能归属于卫星。

| 操作 | Python 常用形式 | 返回值与语义 |
| --- | --- | --- |
| 创建 | `objects.sensor("Camera", parent=..., field_of_view=...)` | 返回带类型句柄；C++ 使用 `create(definition)` 并返回引用 |
| 严格查找 | `objects[id]` | Python 不存在时抛出 `KeyError`；C++ `get(id)` 返回引用或报对象不存在 |
| 可选查找 | `objects.get(id)` | Python 不存在时返回 `None`，也可指定默认值 |
| 读取 | `sensor.name`、`objects.read(sensor)` | 属性读取当前值，`read()` 返回固定的只读状态快照 |
| 筛选 | `objects.find(type=Sensor)` | 返回匹配对象的句柄集合；C++ 返回引用集合 |
| 子对象 | `objects.children(parent)` | 返回直接子对象 |
| 修改 | `sensor.update(**changes)` | 一次校验并修改多个属性；C++ 使用 `update(ref, patch)` |
| 重命名 | `sensor.name = "Camera"` | 保留稳定 ID；公共管理器也提供 `rename(ref, name)` |
| 复制 | `objects.duplicate(sensor, name=...)` | 返回新对象句柄或引用 |
| 重新挂接 | `objects.reparent(sensor, parent)` | 显式修改所属关系 |
| 删除计划 | `objects.plan_remove(sensor)` | 列出删除范围、阻断引用及受影响报告 |
| 删除 | `objects.remove(sensor)` | 按所属关系和引用规则删除 |

普通修改操作不要求调用方处理 `ChangeSet`；界面和其他观察者通过提交后的订阅获得变更。`read()` 的结果不提供直接回写项目的成员引用。Python 不再暴露可随意增删的底层对象列表，避免容器副本与实际项目状态不一致；pybind11 自动 STL 转换的复制语义见[官方文档](https://pybind11.readthedocs.io/en/stable/advanced/cast/stl.html)。

Python 句柄的可写属性和 `update()` 委托所属对象管理器执行公共命令，保持正常的校验、通知与失败行为。句柄持有项目生命周期；在该项目的编辑上下文内读写候选状态，在上下文外读写已提交状态。需要一致读取多个属性时使用 `objects.read(handle)` 捕获一份固定状态。

### 查找与交互体验

```python
sensor = project.objects[sensor_id]       # 必须存在
optional_sensor = project.objects.get(sensor_id)  # 允许缺失

for obj in project.objects:
    print(obj.id, obj.name)

matches = project.objects.find(name="Camera", type=of.objects.Sensor)
```

ID 查询不会退回名称查询；名称允许重复，`find()` 返回全部匹配项。`get()` 的缺省行为采用 Python 用户熟悉的返回默认值约定，见 [dict.get](https://docs.python.org/3/library/stdtypes.html#dict.get)。集合支持 `len()` 和对象迭代，迭代开始时固定成员 ID，属性读取仍遵循当前上下文；需要一致的整组属性时显式取得状态快照。集合不提供下标赋值来替换对象身份。

`repr(sensor)` 显示类型、ID、名称及必要的父对象信息，删除后显示失效状态；显示对象本身不计算轨道或读取外部文件。创建和修改方法提供真实运行时签名及 `.pyi` 声明，不能仅暴露 `**kwargs: Any`。未知参数给出合法参数名称，单位错误同时说明期望量纲与实际输入。

### 身份与生命周期

- ID 自动生成，在项目内跨类型唯一，创建后不可修改，删除后不复用；迁移时保留已有稳定 ID。
- 名称允许重复且可以修改；对象关系、报告绑定和界面选择均使用 ID。
- 运行时引用包含项目实例范围和对象身份，跨项目误用须报错。重新打开文件后按保存的 ID 取得新的引用；Python 已有句柄跟随其所属项目的当前编辑上下文，不需要另取事务句柄。
- 删除后，旧引用或句柄不能继续访问当前项目中的对象。已创建的 `Analysis` 接收句柄时提取其身份，读取自己捕获的旧对象状态，不读取句柄的当前属性。
- 事务内的新引用只对候选状态有效，提交成功后成为当前项目引用；事务丢弃后新对象句柄失效，已有对象句柄恢复读取已提交状态。失效身份不得重新绑定到其他对象。
- 拥有旧快照的读者保持有效，不依赖项目当前容器元素的地址。

### 属性修改

Python 多属性修改直接使用关键字参数，常用写法如下。`sensor.update()` 的这两个字段一起校验、一起提交，任意字段无效时全部保留原值。

```python
from orbitforge.sensors import Cone, Pointing

sensor.update(
    field_of_view=Cone(half_angle=u.deg(15)),
    pointing=Pointing.nadir(),
)
```

单个属性可以直接读写；编辑上下文之外，每条赋值都是一次完整修改，不隐式触发数值计算：

```python
sensor.name = "Camera"
sensor.field_of_view = Cone(half_angle=u.deg(15))
sensor.pointing = Pointing.nadir()
```

上下文之外的多条赋值不自动合成一个原子操作。相互依赖的属性使用一次 `update()`，多对象修改使用 `project.edit()`。管理器也提供 `project.objects.update(sensor, **changes)`，供通用工具使用，与句柄方法调用同一个入口；普通脚本优先使用对象自身的属性和 `update()`。修改方法返回 `None`，创建和查询返回对应对象或结果，不通过返回自身鼓励混合编辑与计算的链式调用。

C++ 及公共命令内部继续使用按对象类型定义的 `SatelliteUpdate`、`GroundStationUpdate` 和 `SensorUpdate`。Python 门面把关键字参数转换为这些命令，普通用户不需要显式构造更新结构。ID 与类型只读，所属关系通过 `reparent()` 单独处理。

| 输入状态 | Python 表达 | C++ 表达 | 行为 |
| --- | --- | --- | --- |
| 未提供 | 省略字段 | 默认更新字段 | 保持原值 |
| 设置 | 具体值 | 具体值 | 替换并校验 |
| 清空 | `None` | `of::clear` | 仅对可空字段合法 |

C++ 更新字段使用三态类型，普通 `optional<T>` 不同时承担“未提供”和“清空”两种含义。轨道、视场与指向等配置按完整变体替换，避免切换模式后残留无效字段。

`sensor.field_of_view` 等嵌套配置是不可变值；修改时赋回完整配置，不允许 `sensor.field_of_view.half_angle = ...` 这种可能只修改临时副本的写法。可写顶层属性、关键字更新与 C++ 更新字段保持相同语义。

Python 的句柄属性和 `update()` 提供按对象类型区分的签名与类型声明，支持 IDE 自动补全。类型或关键字错误在修改前抛出 `TypeError`，未知或只读属性赋值抛出 `AttributeError`；单位、范围和关系错误进入公共校验。不能吞掉拼写错误，也不让调用方通过无约束字典绕过校验。需要重载的通用接口按 [Python typing 规范](https://typing.python.org/en/latest/spec/overload.html)声明，运行时行为与静态签名保持一致。

`Pointing.nadir()` 沿用指向地心的定义，`Pointing.track(station)` 表示同历元的理想目标跟踪；API 重构不增加姿态机动、转速上限或安装偏移模型。

### 所属关系与计算引用

| 关系 | 示例 | 删除行为 |
| --- | --- | --- |
| 所属关系 | 卫星拥有传感器 | 删除卫星时一并删除其传感器 |
| 计算引用 | 传感器跟踪地面站 | 引用存续时阻止删除地面站；可在同一事务中解除引用或删除依赖对象 |
| 报告绑定 | 报告选择某颗卫星 | 保留报告配置，生成时明确提示对象缺失 |

复制卫星时复制子传感器并生成新 ID，子传感器改为归属于新卫星，外部地面站目标保持原引用。单独复制传感器保留其父卫星。外部模型路径继续引用原资源，不复制磁盘文件；已有报告继续绑定原对象。

重新挂接必须验证允许的父类型与所属关系，不能产生环。删除计划绑定生成时的项目修订；确认期间项目有变化时重新生成计划，不能直接执行过期的删除范围。

## 事务与发布

`project.edit()` 捕获已提交的基准修订，修改写入私有候选状态。提交校验最终对象集合，因此可以在同一事务中删除地面站及其依赖传感器，而不受操作中间状态影响。

提交顺序为：候选修改、完整校验、基准修订检查、一次性发布、发送变更通知。

- 上下文外的创建、属性赋值和 `update()` 等内部使用单次事务，普通修改不要求用户手工写 `with`。
- Python `with project.edit():` 将当前调用上下文中该项目的对象创建、读取和修改路由到同一候选状态，原有句柄可以直接继续使用。上下文外的读者仍看到已提交状态。
- Python `with` 正常结束时提交，异常时丢弃；C++ 显式 `commit()`，析构只丢弃未提交内容。
- 同一项目的提交串行处理；基准修订已变化时返回冲突，第一阶段不做自动合并或嵌套事务。
- 无实际变化的提交不增加修订、不标记未保存，也不发送变更通知。
- 一次成功提交产生一个新修订和一个 `ChangeSet`，包含新增、删除、修改的 ID、属性路径及前后修订。
- 提交后的观察者错误不能回滚已经成功的提交；通知与副作用需要各自处理失败。
- 高频小修改应复用未变化的对象快照，避免每次复制整个项目；性能收益通过固定输入测量。

```python
with project.edit():
    sensor.name = "Camera"
    sensor.update(
        field_of_view=Cone(half_angle=u.deg(15)),
        pointing=Pointing.nadir(),
    )
    station.minimum_elevation = u.deg(10)
```

块内可以读到自己刚写入的值，离开块时一起校验和提交。某次调用失败时该次修改不进入候选；异常离开上下文或提交失败时丢弃整个候选，已有对象恢复原状态。C++ 保留显式的 `edit.objects()` 和 `edit.commit()`，Python 上下文路由属于门面，底层仍使用同一事务实现。

路由按项目和调用上下文隔离，并绑定进入事务的线程与异步任务身份。实现可使用 [contextvars](https://docs.python.org/3/library/contextvars.html)保存路由，但仍须校验所有者；Python 新建异步任务会复制上下文，[asyncio.to_thread](https://docs.python.org/3/library/asyncio-task.html#asyncio.to_thread)也会传播上下文，这不表示可以共用可变事务。错误共享时明确拒绝，退出及失败时恢复原路由，避免后续调用误入已关闭事务。通知观察者前须解除候选路由，观察者读取已提交状态。

事务只包含所属项目，不提供跨项目原子提交。第一阶段不支持同项目嵌套事务；Python 的项目保存和分析准备放在编辑上下文之外，在未结束编辑中调用时给出明确错误，不隐式提交草稿。桌面内部的候选准备另由显式协调器执行。

普通 SDK 用户可以提交尚未计算的合法项目，再显式准备分析。桌面“应用”在提交前还需准备计算与显示资源：

```text
编辑器草稿
  → 公共校验与数据检查
  → 必要的数据差异确认
  → 后台准备计算与显示资源
  → 检查基准修订、草稿版本和取消状态
  → 一次性提交项目并发布显示
```

协调器持有同一份候选状态及准备结果。失败、取消或冲突时丢弃候选内容，保留原活动场景。无需重算的修改只准备其实际依赖；名称修改不触发轨道准备。GPU 操作由桌面协调器及渲染适配层负责，公共模型不依赖 Qt 或图形库。

## Python 使用示例

以下为目标工作流。示例目录应已安装覆盖该时段的数据；`snapshot()`、`prepare()` 和查询不会隐式下载。

```python
import orbitforge as of
from orbitforge import units as u
from orbitforge.sensors import Cone, Pointing

data = of.DataCatalog.open("local/data").snapshot()
t0 = data.time.parse_utc("2026-10-01T00:00:00Z")

project = of.Project.create(
    name="Observation Demo",
    interval=of.Interval(t0, t0 + u.h(2)),
)

initial = of.orbits.Keplerian(
    epoch=t0,
    frame=of.Frame.GCRF,
    semi_major_axis=u.km(7000),
    eccentricity=0.001,
    inclination=u.deg(53),
    raan=u.deg(0),
    argument_of_periapsis=u.deg(0),
    true_anomaly=u.deg(0),
)

with project.edit():
    satellite = project.objects.satellite(
        "Observer",
        orbit=of.orbits.TwoBody(initial=initial),
    )

    station = project.objects.ground_station(
        "Hong Kong",
        location=of.Wgs84Location(
            longitude=u.deg(114.2),
            latitude=u.deg(22.3),
            ellipsoid_height=u.m(350),
        ),
        minimum_elevation=u.deg(5),
    )

    sensor = project.objects.sensor(
        "Camera",
        parent=satellite,
        field_of_view=Cone(half_angle=u.deg(20)),
        pointing=Pointing.track(station),
    )

analysis = project.prepare(data=data)

trajectory = analysis.states(
    satellite,
    step=u.s(30),
    frame=of.Frame.GCRF,
)

windows = analysis.access(
    station,
    sensor,
    search_step=u.s(1),
    boundary_tolerance=u.ms(1),
)

xyz_km = trajectory.position.to_numpy(unit="km")
for window in windows:
    print(window.start, window.end, window.duration)

trajectory.to_csv("trajectory.csv")
project.save("observation.orbitforge", analysis=analysis)
```

## C++ 使用示例

C++ 已提供不依赖 Qt 的聚合头文件和 `of::Project`。以下为当前可用接口，创建方式与 Python 项目构造器一致，候选事务仍须显式提交。

```cpp
#include <orbitforge/orbitforge.hpp>
namespace of = orbitforge;
using namespace std::chrono_literals;

int main() {
    of::Project project{"Observation", of::Epoch{0}, of::Epoch{120'000'000'000}};
    auto edit = project.edit();
    auto satellite = edit.objects().create(of::objects::Satellite{"Observer", {}, {}});
    auto sensor = edit.objects().create(of::objects::Sensor{"Camera", satellite, 0.2, {}});
    edit.commit();

    auto analysis = project.prepare();
    auto trajectory = analysis.states(satellite, 30s, of::FrameId::gcrf);
    project.save("observation.orbitforge", analysis);
    auto reopened = of::Project::open("observation.orbitforge");
}
```

C++ `objects::Satellite` 当前接受 `OrbitInput`，位置、速度、角度使用明确的 SI 字段；时长使用 `std::chrono`。此前设想的 `of::orbits`、`of::units` 和强类型视场配置仍属后续设计。完整当前签名、对象身份、目录选择及确认规则见 [C++ 项目 API](cpp_api.md)。Python 继续使用关键字参数与单位值；两端复用同一文档、校验、分析和持久化实现，不要求逐字对应的语法。

## Python 参数与结果约定

配置属性、创建参数和 `update()` 参数使用相同名称。名称可作为创建方法的第一个位置参数；创建和修改时的角度、高度、历元、父对象与目标等使用关键字参数，避免交换同类型参数后仍能运行。查询允许用位置参数传入明确的主对象，时间、参考系和其他选项使用关键字。`field_of_view`、`minimum_elevation` 等名称不再并行增加缩写别名。

长度、角度和时长继续使用 `u.km(7000)`、`u.deg(53)`、`u.s(30)` 等明确值；带量纲字段不根据裸数字猜测单位。`Wgs84Location` 保留大地纬度和椭球高的明确含义。配置值可复用且不可变，对象属性是受管理的修改入口，结果是固定输入的只读输出。

初始轨道、场景区间和视场等决定计算含义的输入必须提供。与当前模型一致的有限默认值可以保留，例如地面站最低仰角为 0、传感器指向为地心天底方向；默认值进入实际配置、可读取并在签名中说明。参考系在示例中显式给出，脚本不读取桌面的单位或坐标系偏好。

| 查询 | Python 结果 |
| --- | --- |
| `analysis.state(satellite, at=epoch, frame=...)` | 单个带历元、原点、参考系的状态 |
| `analysis.states(satellite, step=..., frame=...)` | 分析区间内规则采样的状态序列 |
| `analysis.states(satellite, times=epochs, frame=...)` | 按显式历元查询的状态序列，保留输入顺序与重复历元 |
| `analysis.access(station, sensor, search_step=...)` | 可迭代的可见区间，每项有 `start`、`end`、`duration` |

`step` 与 `times` 互斥，省略二者时报缺失参数；显式历元必须位于已准备区间，空历元序列返回带完整类型信息的空结果，不改为查询整个区间。单历元与序列查询返回类型固定，不能因为结果只有一行就自动降维。状态序列的数值列使用名称访问，例如 `trajectory.position`，其 NumPy 导出始终为 `(N, 3)`，包括 `(0, 3)` 和 `(1, 3)`，时间列长度为 `N`。`len(trajectory)` 表示采样数，`len(windows)` 表示区间数。

常用结果直接提供 `to_csv()`，使用结果类型对应的默认呈现，不要求构造报告模板，也不重新计算。默认 CSV 数值列注明 SI 单位，时间列保留精确的 TAI 纳秒；模板可以增加 UTC 显示列。数值数组通过 `to_numpy(unit=...)` 显式转换，表格分析可以通过 `to_pandas()` 获取副本。NumPy 与 pandas 为适配能力；未安装对应依赖时仅相关转换方法报出清晰的 `ImportError`，核心项目操作和 CSV 导出不依赖它们。

```python
xyz_km = trajectory.position.to_numpy(unit="km")
trajectory.to_csv("trajectory.csv")
table = trajectory.to_pandas()
```

pandas 默认表保留精确的 `epoch_tai_ns` 整数列，展开的数值列采用 SI，并附带单位和参考系元数据；UTC 可作为使用同一时间数据生成的显示列。考虑到 [NumPy datetime64 的闰秒限制](https://numpy.org/doc/stable/reference/arrays.datetime.html#datetime64-shortcomings)，转换不默认使用普通日期时间索引，也不对时间或数值列静默降精度。用户对导出副本的修改不影响项目或分析。

数据目录、运行时快照和显式分析准备保留可见边界。获取对象、读取属性、显示 `repr` 和修改配置均不联网、不写文件、不触发轨道计算；下载、计算和保存各自有明确入口。脚本遇到数据变化时通过异常取得对应计划，按调用方明确给出的决定处理；库不弹出桌面确认窗口，也不自动同意替换。

## 分析与数据一致性

### 固定输入和能力

`DataCatalog` 管理本机当前数据。`DataSnapshot` 固定本次可选择的数据资源及策略，持有资源生命周期；目录更新或目录对象释放不改变已捕获的资源。这里的快照是运行时概念，不增加历史版本管理界面，也不要求普通项目打开时安装旧文件。

`Analysis` 固定项目输入、所选 Provider、数据解释规则和算法配置。准备完成后明确其可用能力及已校验依赖；查询不能改用后来更新的目录内容。需要新增数据依赖或扩大已准备的区间时，重新准备并执行对应检查。

纯地心 GCRF 传播不强制依赖 EOP 或图形资源；UTC 解析、地固变换及可见性按所需能力校验依赖。桌面额外准备显示资源，无界面计算保持独立。

查询默认使用分析区间，也可指定其子区间。规则采样包含精确终点，非整除步长不丢弃终点；显式 `times` 只查询给定历元，不额外插入端点，单历元状态查询合法。Access 的搜索步长与边界细化容限分别定义，短于搜索步长的窗口或间隙仍可能遗漏，沿用现有算法限制。

公共量纲类型在 API 边界接受显式单位并转换为 SI。时间保持以 2000-01-01 12:00:00 TAI 为零点的精确纳秒表示；状态保留历元、原点和参考系。二体瞬时根数与 J2/J4 平均根数使用不同输入类型，SGP4 以 TLE 为轨道输入，不混用非活动手工根数。

### 打开与重新计算

已有项目使用 `Project.plan_open(path, data=...)` 或 `project.plan_prepare(data=...)` 生成计划。计划固定已解析的文件或候选项目输入、基准修订、所选数据和策略，并提供数据差异与阻断诊断。

| 情况 | 行为 |
| --- | --- |
| 所需数据一致且有效 | 可以继续准备 |
| 数据身份或实际使用策略变化 | 展示差异，取得同意后执行该计划 |
| 用户拒绝 | 停止打开或替换，保留原活动场景 |
| 覆盖不足、格式错误或 UTC 失效 | 阻止操作并显示具体原因；覆盖错误附所需范围、可用范围和处理入口 |
| 确认期间目录更新 | 仍执行已确认计划固定的数据；采用更新数据须重新生成计划 |
| 准备期间项目或草稿改变 | 发布时拒绝过期候选；分析快照可作为旧结果保留 |

直接调用 `open()` 或 `prepare()` 遇到需要确认的变化时默认抛出 `DataChangedError`，Python 异常的 `plan` 属性提供这次已经固定的计划。应用负责展示计划差异；用户明确同意后，才能调用该计划的 `open(accept_data_changes=True)` 或 `prepare(accept_data_changes=True)`。确认参数不能绕过覆盖或文件完整性错误，也不改变后续计划的默认行为。

保留数据中心的来源优先级规则：默认 C04 优先，完整覆盖候选优先于需要外推的候选。有限 EOP 外推遵守每个来源的显式设置与上限，不改变原始覆盖范围，也不延长 UTC 有效期。

### 溯源与保存

分析与结果记录实际使用的数据内容、解释配置、算法版本和相关输入身份。路径、下载时间、检查时间以及未选用的候选数据不影响数值计算身份；变更现有哈希编码或解释规则须更新版本并拒绝旧格式。

数据依赖哈希与项目计算输入哈希分开，结果还包含查询参数。准备成功只表示该分析配置与依赖有效，不表示所有报告已经计算完成。

`save(..., analysis=analysis)` 检查分析属于该项目且当前相关计算输入与其对应。名称和样式变化允许沿用数值溯源；轨道等输入变化后，不能把旧分析记录成新输入的有效分析。保存未计算项目时保留真实的未计算或已过期状态，不制造成功计算记录。

保存已提交输入、对象关系、样式、报告配置和来源信息，不保存 Provider 指针、运行中任务、GPU 资源或显示缓存。保存和导出继续采用准备、校验、原子发布，默认拒绝覆盖已有目标；调用方须显式指定覆盖选项。

## 界面联动与依赖更新

项目修订用于检测编辑冲突；计算缓存依据相关输入、所选数据、模型和查询参数。仅按全局修订变化清空所有计算缓存会造成不必要的重算。

| 修改 | 更新范围 |
| --- | --- |
| 对象名称 | 对象树、标签及新的报告呈现；已生成结果保留自己的名称快照 |
| 轨道颜色、线型、粗细 | 对应显示资源 |
| 卫星轨道 | 该卫星状态及依赖它的分析 |
| 地面站位置 | 站点显示、相关 AER 与可见性 |
| 地面站最低仰角 | 相关可见性约束与结果 |
| 传感器视场或指向 | 相关传感器显示和可见性，不重算父卫星轨道 |
| 报告单位、列顺序 | 报告呈现 |
| 报告查询步长、区间 | 对应查询结果 |
| 所选时间或 EOP 数据 | 实际依赖这些数据的分析与显示转换 |

对象树按稳定 ID 保留选择、展开状态和搜索条件，按 `ChangeSet` 更新相关行。Qt 适配器持有自己的读取快照，在 GUI 线程中以正确的插入、删除通知包围模型数据变化，不能先替换底层数据再补发结构通知。线程和通知约束见 [QAbstractItemModel](https://doc.qt.io/qt-6/qabstractitemmodel.html)。

属性页绑定编辑草稿，播放时刻、相机、布局和选择属于工作区状态。播放一帧或改变面板宽度不产生对象编辑事务，也不重算轨道；当前时刻仍可作为保存时的视图提示。

后台任务只消费固定输入。发布结果时检查相关依赖身份，旧轨道任务不能覆盖新轨道场景；报告可以保留带过期标记的旧结果供查看，但不将其发布到当前时间轴或地图联动。

## 报告与结果

| 部分 | 内容 |
| --- | --- |
| 查询 | 对象或对象角色、分析区间、采样或搜索步长、坐标系、计算选项 |
| 结果 | 静态属性、时间序列或时间区间，以及单位、时间语义和溯源 |
| 模板 | 字段选择、显示单位、时间格式、列顺序和图表配置 |

`analysis.states()`、`access()`、`aer()` 等便捷方法与类型化查询共用执行入口。普通脚本直接读取或导出结果；需要复用列选择和格式时再创建模板。保存的报告配置组合查询和模板；结果直接导出与显式模板共用格式化实现，不隐式发起新的轨道计算。目前 `analysis.report()` 已将九类内置报告接入固定分析并复用共享表格引擎，自由模板与字段描述表仍按后续阶段推进。

```python
template = of.reports.Template(
    source="orbit.state",
    columns=[
        of.reports.Column("epoch", time_scale=of.TimeScale.UTC),
        of.reports.Column("position.x", unit="km"),
        of.reports.Column("position.y", unit="km"),
        of.reports.Column("position.z", unit="km"),
    ],
)

report = template.apply(trajectory)
report.to_csv("position.csv")
```

模板只接受结果中兼容的字段，缺失字段明确报错。UTC 格式化使用该分析固定的时间数据；缺少转换能力时报告错误，不从当前目录补入另一份数据。

对象右键打开报告默认绑定该对象；“所有兼容对象”采用显式选择模式，空集合表示没有对象。对象删除后保留报告绑定并提示缺失，不改绑父对象或其他对象。Access 和 AER 明确区分地面站与目标角色。

表格与图表使用同一份结果。时间区间采用区间表或时间条，时间序列采用曲线；显示降采样不回写分析结果。Python NumPy 转换默认提供明确的复制语义，单位转换产生独立数据；未来如提供无复制视图，必须固定存储所有者。C++ 只读视图的有效期依附于拥有数据的结果对象。

## 数据驱动范围

第一阶段由程序注册内置描述表，使用稳定类型 ID 和字段 ID，与中文标签分离。

| 描述 | 主要内容 | 使用方 |
| --- | --- | --- |
| 对象类型 | 名称、图标、允许父类型、属性分组 | 插入菜单、对象树、对象选择窗口 |
| 属性 | 类型、量纲、范围、默认值、编辑方式 | 常规属性编辑器、公共校验 |
| 传播器 | 输入类型、模型语义、支持能力和数据依赖 | 轨道编辑器、分析准备 |
| 查询 | 对象角色、参数、结果形态、依赖 | 报告选择、查询执行和失效判断 |
| 报告字段 | 字段 ID、量纲、格式和绘图能力 | 模板编辑器、表格和曲线 |
| 数据源 | 来源、格式、覆盖信息和更新能力 | 数据中心与下载管理 |

常规表单读取描述信息；轨道、历元和传感器等复杂编辑器继续专门设计，并提交相同的配置与命令。公共校验器处理跨字段与物理约束，界面不另外维护一份有效性定义。查询实现明确声明计算依赖，不能仅凭属性标签推断缓存有效性。

对象选择继续使用可搜索窗口，按业务角色和能力筛选候选对象。变成数据驱动不改变已确定的响应式布局、草稿保留及对象绑定契约。

## 模块与错误边界

| 层 | 职责 |
| --- | --- |
| Core 与 Results | 数学、时间、状态、基础接口及拥有存储的结果 |
| Application | 对象模型、事务、关系规则、公共校验、变更通知和持久化配置 |
| Runtime 与 Provider | 具体计算适配、数据资源、分析准备和查询执行 |
| 公共 API 门面 | 将模型与 Runtime 组合成 `Project` 和常用工作流 |
| Desktop 与 Python | 界面协调、语言绑定和结果适配 |

依赖继续遵循 Runtime → Application → Results → Core。公共门面位于 Runtime 之上或作为其上层入口，不让 Application 因 `Project.prepare()` 反向依赖 Runtime；门面复用领域模型，不维护另一份项目状态。具体文件划分在实现阶段确定。

C++20 与 Python 使用一致的错误分类：参数与物理输入错误、对象缺失或类型不符、引用阻断、修订冲突、数据变化、覆盖不足、能力缺失和取消。C++ 异常映射为对应 Python 异常；诊断携带稳定错误码、对象 ID、字段路径和覆盖信息，调用方不解析翻译后的错误文案。

表单预校验与实际执行使用相同规则。同步 API 先提供统一的取消与进度约定，由桌面调度后台任务；Python 在输入固定后可释放 GIL 执行计算。失败或取消不交付冒充完整结果的部分数据。

## 文件与 API 迁移

当前 `.orbitforge` 仅读写 v40，包含仅 UTC 的来源及分析准备记录；v1–v26 及未知版本明确拒绝。开发阶段不承诺旧文件兼容，不维护迁移分支，不让旧存储结构约束新 API；结构或语义改变时递增格式版本并整体更新当前实现。API 版本与文件格式分别演进。对象事务、固定分析、目录选源、检查后打开/保存、`analysis.access()` / `aer()` 和内置报告查询已实现，当前入口见[分析 API](analysis_api.md)；查询/报告模板分离与显式对象范围已落地；统一 C++ 顶层门面已落地；C++ 强类型配置、桌面计划接入继续按阶段推进。

- 保留稳定 ID、所属和目标引用、名称、显示设置、时间精度、轨道输入及数据来源信息。
- 保留瞬时根数、平均根数和 TLE 的物理含义；SGP4 的非活动手工草稿不得误作计算输入。
- 报告使用显式选择模式：`objects` 的空集合报错，`all_compatible` 才表示全部适用对象；Access 和 AER 保留明确的对象对。当前 v40 已统一保存此语义以及显示模板，仅保留当前格式读写。
- 更改哈希规范编码与解释规则时使用新版本，只校验当前记录并拒绝旧格式，不自动迁移。
- 非当前文件版本、未知类型和缺失必需字段明确拒绝，避免读取后静默丢失信息。
- 按阶段同步 C++ 导出、Python 绑定与类型声明、SDK 消费端、示例和桌面调用方。必要的过渡别名委托新实现，不保留两套业务逻辑；尚未承诺稳定 ABI。

## 实施顺序与验收

| 阶段 | 主要交付 | 验收重点 |
| --- | --- | --- |
| 接口契约 | 类型、方法、单位、错误和跨语言示例 | Python 常用操作简洁且可补全，两端行为一致，现有能力有明确映射 |
| 对象模型与事务 | 公共对象存储、命令、关系校验和 `ChangeSet` | 原子修改、回滚、冲突、引用及复制行为 |
| C++ 与 Python API | 公共门面、绑定、分析快照和数据确认计划 | 等价输入、资源生命周期、数据差异与覆盖错误 |
| 桌面联动 | 草稿与准备流程、增量对象树、按依赖更新 | 失败保留原场景，过期任务不覆盖新状态 |
| 报告与描述表 | 查询和模板分离、能力描述、当前格式调整 | 对象范围、字段语义、版本拒绝与导出一致性 |

第一批实现以统一对象管理、事务和变更通知为边界。每阶段通过主题分支与 PR 交付，遵循[开发者规范](../development/requirements.md)，不将完整重构集中成无法分步验证的单次替换。

关键验收用例包括：

- Python、C++ 和桌面的等价操作产生一致的已提交输入与对象关系。
- Python 属性赋值、句柄 `update()` 与管理器更新走相同校验；多字段失败整体回滚，省略与清空语义正确，拼写错误和只读属性赋值不会被忽略。
- Python 创建方法与更新签名可自动补全，运行时签名和类型声明一致；`objects[id]`、`get()`、重复名称搜索和失效对象行为符合约定。
- Python 原有句柄在编辑上下文内读写候选，正常退出一起提交，失败恢复原状态；跨线程或任务误用事务不会被上下文传播掩盖。
- 多项修改失败时不留部分对象；无操作提交不发送通知；过期编辑不覆盖新修订。
- 删除卫星连带子传感器，地面站引用阻止非法删除，复制正确重映射内部引用。
- 失效引用明确报错，旧分析在当前对象被删除后仍保留原状态与资源。
- 确认期间下载新数据不改变已经确认的计划；拒绝、覆盖失败和取消不打开或替换场景。
- 改名和样式不重算轨道，传感器配置变化不重算父卫星轨道，依赖变化仍能使对应结果失效。
- 输入单位、参考系、闰秒边界、采样终点及 Access 搜索限制符合现有数值契约。
- 当前格式往返保存保持语义，非当前版本明确拒绝；显式全选与空选正确区分。
- NumPy 数组、C++ 只读视图、分析和数据资源的生命周期在异常与取消路径上仍然有效。
- 单历元与序列查询返回类型固定，显式时刻的顺序与重复值保持不变；CSV 默认导出不触发计算，缺少可选 Python 分析依赖不影响核心功能。
