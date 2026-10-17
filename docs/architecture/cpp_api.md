# C++ 项目 API

日常 SDK 使用 `<orbitforge/orbitforge.hpp>`，链接 `OrbitForge::Runtime`。`orbitforge::Project` 把对象编辑、分析准备、数据确认和项目保存连接起来，内部直接使用与 Python 相同的 Application 文档和 Runtime 计划。公共头文件不依赖 Qt、Python、VSG 或 Rocky；专用 Provider 接口仍使用各自模块头文件。

## 创建、编辑与查询

```cpp
#include <orbitforge/orbitforge.hpp>

namespace of = orbitforge;
using namespace std::chrono_literals;

int main() {
    of::Project project{"Observation", of::Epoch{0}, of::Epoch{120'000'000'000}};
    auto edit = project.edit();
    auto satellite = edit.objects().create(of::objects::Satellite{"Observer", {}, {}});
    auto station = edit.objects().create(of::objects::GroundStation{
        .name = "Station", .location = {0, 0, 100}, .minimum_elevation_rad = 0.1,
    });
    auto sensor = edit.objects().create(of::objects::Sensor{
        .name = "Camera", .parent = satellite, .half_angle_rad = 0.2,
        .pointing = {station}, // 空 Pointing{} 表示对地心。
    });
    edit.commit();

    project.objects().update(sensor, of::objects::SensorUpdate{.half_angle_rad = 0.15});
    auto analysis = project.prepare();
    auto trajectory = analysis.states(satellite, 30s, of::FrameId::gcrf);
    auto report = analysis.report(
        {.kind = of::ReportKind::cartesian_state, .object_ids = {satellite.id()}},
        {.columns = {{"time.epoch", "", "TAI"}, {"state.x", "km", "Position X"}},
         .time_scale = of::ReportTimeScale::tai});

    project.save("observation.orbitforge", analysis);
    auto reopened = of::Project::open("observation.orbitforge");
    auto restored_satellite = reopened.objects().get(satellite.id());
}
```

该示例不需要外部数据文件；默认卫星的通用二体传播需要启用 CSPICE（`ORBITFORGE_WITH_SPICE=ON`，如 `astronomy` 预设）。创建自定义轨道时传入 `of::OrbitInput`，其 `initial` 为 SI 制 Cartesian 状态。地面站经纬度、最低仰角、传感器半锥角均为弧度，椭球高为米。`Epoch` 为从 2000-01-01 12:00:00 TAI 起算的 int64 纳秒，采样步长使用 `std::chrono` 整数时长。C++ 强类型轨道配置、单位构造器仍待后续实施，不应把 Python 的 `of.orbits` / `of.units` 示例直接当成 C++ 接口。

可运行示例为 [`examples/cpp/project_analysis.cpp`](../../examples/cpp/project_analysis.cpp)，启用 CSPICE 时生成 `orbitforge_project_example`。独立安装验证 [`sdk_consumer/project_api.cpp`](../../tests/packaging/sdk_consumer/project_api.cpp) 仅包含聚合头文件，通过地面站 ITRF 状态完成编辑、查询、报告、保存与重开；这条基础流程不依赖天文库。

HPOP 的球形太阳辐射压使用 `OrbitInput.hpop.srp`（`core::SrpOptions`）；无阴影、圆柱和双锥枚举为 `core::SrpShadowModel`。Cr、面积质量比、半径、光压与参考距离均为 SI，详见[光压契约](solar_radiation_pressure.md)。即使所有第三体引力关闭，启用光压仍须通过 `DataSnapshot` 提供太阳星历；月球遮挡启用时还须月球。分析、报告和保存时捕获同一不可变数据身份，低层几何检查可直接调用 `core::evaluate_srp`。

## 项目与对象的所有权

| 操作 | 语义 |
| --- | --- |
| `auto other = project` | 共享同一可编辑文档、对象身份、修订与订阅 |
| `project.snapshot()` | 固定修订的只读快照；后续编辑不改变它 |
| `project.to_data()` | 独立的 `application::Project` 值；修改不会回写 |
| `Project::from_data(data)` | 校验并创建独立文档；保留对象 ID，分配新的项目身份 |
| `Project::from_document(document)` | 适配层桥接；共享现有 `ProjectDocument`，不重复导入 |
| `project.document()` | 显式取得底层文档供适配层使用；普通 SDK 使用项目方法即可 |
| `Project::open()` / `plan.open()` | 每次返回独立文档；需按 ID 重新取得该项目的对象引用 |

`ObjectRef` 带项目身份，跨文档使用会报错，不能仅凭相同 ID 借用对象。`Analysis` 持有固定输入与资源，原项目、目录或文件释放后仍可查询。`project.name()` / `first()` / `last()` / `current()` 是便捷读取；需要一组一致的值时，从一次 `snapshot()` 读取。

C++ 事务显式操作 `edit.objects()`、`edit.settings()`，提交用 `commit()`，析构丢弃未提交草稿。`project.objects()`、`prepare()` 和 `save()` 始终操作已提交文档，不会自动路由到局部事务。对已提交项目的另一次编辑会使旧事务 / 准备计划发生修订冲突。Python 的 `with project.edit():` 上下文路由是 Python 特有的便利机制。

项目设置可以一次修改多个字段：`project.update({.name = "Mission", .last = end})`。省略的字段保留原值，失败时整次修改不发布。报告定义通过 `set_reports()` 原子替换。`subscribe()` 返回用于 `unsubscribe()` 的令牌；通知、异常隔离与线程规则沿用[对象编辑契约](object_editing.md)。

## 数据选择与确认

跨参考系、Access 和 AER 查询需要明确提供数据。例如，使用已有本地数据目录：

```cpp
of::DataCatalog catalog{"local/shared-data"};
auto analysis = project.prepare(catalog);
auto trajectory = analysis.states(satellite, 30s, of::FrameId::itrf);
auto windows = analysis.access(station, sensor, {.search_step = 1s});
auto aer = analysis.aer(station, sensor, 10s);
```

也可以用 `DataSnapshot::load(eop_manifest, time_file, extrapolation_days)` 加载固定资源，或者使用 `DataSnapshot::builtin_time()` 进行仅需 UTC 的计算。目录选源、覆盖检查、外推和错误规则见[分析 API](analysis_api.md)；这些操作不联网。

已有 Provider 的 C++ 适配层可调用 `DataSnapshot::from_providers(time, frames, eop)`，参数为 `shared_ptr<const ...>`。快照保留原实例和资源所有权，不重新加载文件；调用方须保证 Provider 不可变且支持并发读取。允许全部为空、仅时间、完整三件套；缺项或 EOP 与时间表身份不一致时抛出 `std::invalid_argument`，准备分析时另行检查完整时段覆盖与数据变化。这个底层适配入口仅提供给 C++；Python 继续通过目录、文件或内置时间表创建快照。

| 场景 | 入口 | 数据变化时的异常及后续操作 |
| --- | --- | --- |
| 准备分析 | `project.prepare(data)` | `of::DataChangedError`；取得 `error.plan()` |
| 预先检查准备 | `project.plan_prepare(data)` | 返回 `of::AnalysisPlan`，读取 `data_changes()` 后调用 `prepare()` |
| 打开文件 | `of::Project::open(path, data)` | `of::OpenDataChangedError`；取得 `error.plan()` |
| 预先检查打开 | `of::Project::plan_open(path, data)` | 返回 `of::ProjectOpenPlan`，读取 `data_changes()` 后调用 `open()` |

上述 `data` 可以是 `DataSnapshot` 或 `DataCatalog`。只有调用方取得本次明确同意后，才对同一计划调用 `prepare(true)` 或 `open(true)`；拒绝则丢弃计划。类型区分准备和打开两个结果，`OpenDataChangedError::plan().open(true)` 返回公开的 `of::Project`。接受不会变成以后自动同意的设置，也不会重新读取磁盘。计划创建时已经校验资源与覆盖，错误不能通过接受参数绕过。

`save(path)` 保存当前已提交输入，`save(path, analysis)` 另外记录匹配的分析来源。分析必须属于同一文档且物理输入仍一致；改名、样式与报告修改不使分析失效。文件已存在时默认报错；允许覆盖需显式传 `true`。写入失败不改变原文件、内存来源或已保存标记。文件使用当前 v40，包含 HPOP 配置。

## 安装后链接

```cmake
find_package(OrbitForge CONFIG REQUIRED COMPONENTS Runtime)
add_executable(mission main.cpp)
target_link_libraries(mission PRIVATE OrbitForge::Runtime)
```

组件层的 `application::ProjectDocument` 与 `runtime::prepare()` 继续作为适配层入口，门面没有第二套校验、计算或持久化实现。桌面报告已通过 `Project::from_data()` 捕获已发布场景，在后台准备 `Analysis` 并生成结果；地图可见性联动使用该分析捕获的资源。工作台的对象应用与图形场景准备仍需进一步接入同一文档、事务和计划，当前报告捕获的文档修订不代表整个工作台的编辑次数。

大气阻力通过 `HpopOptions.drag` 接入，低层 `evaluate_atmosphere` / `evaluate_drag` 使用 SI 输入；前者的 NRLMSISE 可显式传入 `AtmosphereInput.daily_ap` 或 `ap_history`（互斥），后者仍使用手动 Kp；实际 HPOP 同时保留 UTC/EOP/Sun。模型、跨接口和独立参考见[阻力契约](../architecture/atmospheric_drag.md)及[阻力验证](../validation/atmospheric_drag_protocol.md)。


### 海潮资源

聚合头 `orbitforge/orbitforge.hpp` 导出 `core::OceanTideOptions`、`OceanTideModel` 与 `runtime::load_ocean_tides`。HPOP 设置 `ocean_tides = {true, "fes2014bv1", 4, 4}`；资源通过 `DataSnapshot::with_ocean_tide_file(id, path)`、`with_ocean_tides(id, shared_ptr<const OceanTideModel>)` 或目录安装注入。资源只读并由分析共享持有，核心传播不读文件、不联网。关闭或阶数小于 2 时无需海潮数据；活动阶次超出资源范围明确失败。GM/半径缩放、时间和 EOP 约定见[海潮契约](ocean_tides.md)。
