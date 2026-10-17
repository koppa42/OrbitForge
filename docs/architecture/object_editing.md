# 对象编辑 API

本接口实现[统一对象 API 设计](object_api_design.md)中的对象管理与事务阶段。Python 的 `Project` 已使用公共 C++ `application::ProjectDocument` 管理修改；桌面仍使用原工作流，后续接入同一变更通知。[固定输入分析与准备计划](analysis_api.md)已接入，Qt 模型适配和描述表驱动的属性页仍待实施。

## Python 常用操作

```python
import orbitforge as of
from orbitforge import units as u
from orbitforge.sensors import Cone, Pointing

project = of.Project("Observation", of.Epoch(0), of.Epoch(7_200_000_000_000))
initial = of.orbits.Keplerian(
    epoch=project.first,
    semi_major_axis=u.km(7000),
    eccentricity=0.001,
    inclination=u.deg(53),
)

with project.edit():
    satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(initial=initial))
    station = project.objects.ground_station(
        "Hong Kong",
        location=of.Wgs84Location(
            longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350),
        ),
        minimum_elevation=u.deg(5),
    )
    sensor = project.objects.sensor(
        "Camera", parent=satellite,
        field_of_view=Cone(half_angle=u.deg(20)),
        pointing=Pointing.track(station),
    )

sensor.name = "Narrow camera"
sensor.update(field_of_view=Cone(half_angle=u.deg(15)), pointing=Pointing.nadir())

with project.edit():
    sensor.field_of_view = Cone(half_angle=u.deg(10))
    station.minimum_elevation = u.deg(10)

project.save("observation.orbitforge")
```

`objects.satellite()`、`ground_station()`、`sensor()` 返回带类型的实时句柄。通用工具也可以构造 `of.objects.Satellite`、`GroundStation`、`Sensor` 不可变配置，再调用 `objects.create(definition)`。配置与句柄是不同类型；创建、更新方法有真实参数签名，随包提供类型提示。

轨道入口提供 `orbits.TwoBody(initial=...)`、`J2(initial=...)`、`J4(initial=...)`、`Sgp4(tle=...)`，返回不可变 `orbits.Orbit` 配置。二体初值可用 GCRF `Keplerian`、不可变 `orbits.Cartesian` 或既有 `CartesianState`；J2/J4 明确接受各自平均根数。`Orbit.from_input(OrbitInput)` 是旧 SI 输入的显式转换入口，会丢弃旧对象 ID 和分析时段，由项目分配与维护。轨道转换沿用已有算法，不隐式下载时间或 EOP 数据。

可替换的显示值是 `OrbitStyle`，例如 `satellite.display = of.OrbitStyle(line_color=0x73CDEE, line_width=2, line_style=of.LineStyle.DASHED)`。嵌套配置不可变；修改视场、位置、轨道或显示时替换完整值，不修改临时容器副本。模型文件配置暂通过低层项目值导入，句柄能读取，后续扩展属性编辑。

## 查找与生命周期

| 操作 | 结果 |
| --- | --- |
| `project.objects[id]` | 按 ID 查找；缺失抛 `KeyError`，不会回退到名称 |
| `project.objects.get(id, default)` | 缺失返回默认值，默认是 `None` |
| `project.objects.find(name="Camera", type=of.objects.Sensor)` | 返回所有匹配句柄；名称可以重复 |
| `project.objects.read(sensor)` | 返回固定、不可变的对象状态 |
| `project.objects.children(satellite)` | 返回卫星的直接子传感器 |
| `project.objects.duplicate(satellite, name="Copy")` | 复制卫星及子传感器，分配新 ID，保留外部目标引用 |
| `project.objects.reparent(sensor, satellite)` | 显式改变所属卫星 |
| `project.objects.plan_remove(obj)` | 预览当前修订的删除范围、阻断传感器和受影响报告 |
| `project.objects.remove(obj)` | 重新校验并执行删除；预览不是可绕过校验的授权令牌 |

集合支持 `len()` 与迭代；迭代开始时固定成员，属性读取仍取当前编辑上下文中的值。删除后 ID 仍可读取，其他实时属性抛 `ObjectNotFoundError`，`repr()` 显示 `deleted`。旧状态快照仍有效。句柄持有所属项目生命周期，跨项目使用会报错；重新打开文件后按 ID 取得新句柄。

删除卫星时一并删除其传感器。跟踪地面站的传感器会阻止该地面站被删除；可以在一次事务中删除地面站并把相关传感器改为天底指向，最终提交时检查引用完整性。已有报告保留绑定 ID，后续生成时提示对象缺失，不自动更换对象。

## 事务与通知

上下文外，每次赋值或 `update()` 是一个完整事务。`with project.edit():` 内继续使用同一项目和已有句柄，立即读取候选值，正常退出一次性提交；异常或提交校验失败则丢弃全部修改。单个操作失败不会破坏候选值，捕获异常后可继续编辑。

同一项目不支持嵌套编辑。另一个线程可独立修改已提交项目，原候选提交时会抛 `RevisionConflictError`，不会自动覆盖并发修改。`asyncio.create_task()` 和 `to_thread()` 继承的上下文不能访问父任务的活动草稿。保存和计算必须在编辑上下文外执行。

```python
token = project.subscribe(lambda change: print(change.revision, change.added, change.updated))
sensor.name = "Renamed"
project.unsubscribe(token)
```

一次实际提交产生一个 `ChangeSet`，无净变化时不增加修订、不修改 `dirty`、不通知。字段按 `name`、`orbit`、`display`、`location`、`minimum_elevation`、`parent`、`field_of_view`、`pointing` 等分类。通知持有 `before`、`after` 两个精确修订快照；观察者继续编辑后，仍可读取对应事件的输入。通知在发布后于提交线程运行，嵌套产生的通知按提交顺序排队。观察者异常不会撤销提交，错误可从 `project.observer_errors` 读取。Qt 接入时须转发到 GUI 线程。

## C++ 应用层入口

普通 SDK 使用 [C++ 项目门面](cpp_api.md)的 `of::Project` 与 `of::objects`，编辑命令、引用和事务直接复用下列应用层实现。以下入口供需要显式管理底层文档的适配层使用。

```cpp
#include <orbitforge/application/project_document.hpp>

namespace app = orbitforge::application;
using orbitforge::core::Epoch;

app::ProjectDocument project{app::Project{"Observation", Epoch{0}, Epoch{7'200'000'000'000}}};
auto edit = project.edit();
auto objects = edit.objects();
auto satellite = objects.create(app::objects::Satellite{"Observer", {}, {}});
auto sensor = objects.create(app::objects::Sensor{"Camera", satellite, 0.35, {}});
edit.commit();

auto before = project.snapshot(); // revision + shared_ptr<const Project>
project.objects().update(sensor, app::objects::SensorUpdate{.half_angle_rad = 0.2});
```

`ObjectRef` 是带项目范围和类型的值身份，不指向容器元素。C++ 的候选编辑必须显式使用 `edit.objects()` 并 `commit()`，析构时丢弃未提交修改。编辑对象本身仅供一个线程使用；发布状态、独立候选提交和订阅表受锁保护。

当前实现共享不可变项目快照，每次候选操作复制项目并复用现有校验器，提交比较持久化输入并生成字段差异。它建立正确性基线，尚未实现按节点结构共享或增量校验，不能据此宣称万对象批量编辑已有性能保证。后续在接口契约不变的前提下增加索引、结构共享和计算失效范围。

## 迁移与当前计算入口

Python `of.Project` 和 `of.load_project()` 现在返回事务门面。旧的可变项目值改名为 `of.ProjectData`，文件层入口为 `of.load_project_data()`；原生文件格式回归测试继续验证这些低层值。需要与旧集成交换时使用 `Project.from_data(data)` 和 `project.to_data()`，两者均复制，修改副本不会回写项目。

普通脚本不再修改 `project.satellites` 等列表。通过句柄操作对象；保存报告配置使用 `project.set_reports(definitions)`，读取旧配置可从显式 `to_data()` 副本取得。`project.earth_orientation = ...` 继续接受已明确选定的数据元信息，`None` 清空；`plan_prepare(data=...)` 提供数据确认计划，可传入固定的 `DataSnapshot` 或由 `DataCatalog` 按项目时段选源。

`Project.open(path, data=...)` 校验所选数据，变化时通过 `DataChangedError.plan` 取得固定的打开计划；原始输入读取使用 `load_project_data()`。`save(path, analysis=...)` 可记录匹配的分析来源，普通保存保留现状。状态查询优先使用 `project.prepare()` 返回的 `Analysis`；`to_scenario(time=...)`、`generate_report(project, ...)` 和 `AccessSession(project, ...)` 继续捕获已提交输入，执行已有计算与数据匹配规则。文件包含仅 UTC 的来源和分析准备记录，以及报告查询/模板配置；仅读写当前 v40，旧格式明确拒绝，详见[分析 API](analysis_api.md)。

完整示例见 [可见性分析](../../examples/python/access.py)和[保存项目](../../examples/python/save_project.py)。验证入口为 C++ `ProjectDocument.*` 集成测试、Python `test_objects.py`、原生文件与计算回归测试，以及安装后的 SDK 消费端。
