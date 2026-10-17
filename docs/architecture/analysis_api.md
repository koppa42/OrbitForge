# 固定输入的分析 API

在[对象编辑 API](object_editing.md)之上提供共享 C++ Runtime 分析入口。Python 和 [C++ 项目门面](cpp_api.md)均使用 `project.prepare()`，C++ 适配层也可使用 `runtime::prepare(document, data)`。准备时捕获已提交对象、区间、传播器和显式选择的数据；后续对象修改、删除、文件替换或目录释放不会改变已有分析。

## Python 状态查询

```python
import orbitforge as of
from orbitforge import units as u

project = of.Project("Observation", of.Epoch(0), of.Epoch(300_000_000_000))
sat = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
    initial=of.orbits.Keplerian(
        epoch=project.first, semi_major_axis=u.km(7000), eccentricity=0.001,
        inclination=u.deg(53), raan=u.deg(0), argument_of_periapsis=u.deg(0),
    ),
))

analysis = project.prepare()
state = analysis.state(sat, at=project.first, frame=of.FrameId.GCRF)
trajectory = analysis.states(sat, step=u.s(30), frame=of.FrameId.GCRF)
trajectory.to_csv("trajectory.csv")

# 显式历元保持顺序和重复项，不增加额外端点。
selected = analysis.states(sat, times=[project.last, project.first, project.last])

# 修改甚至删除当前对象后，旧分析仍可使用原句柄查询其捕获的对象。
sat.name = "Renamed"
project.objects.remove(sat)
original = analysis.state(sat, at=of.Epoch(0))
```

状态包含历元、地心原点、参考系、米和米每秒数值。二体、J2、J4、SGP4 和 HPOP 复用共用传播器；HPOP 与任务 HPOP 滑行使用[有界科学插值星历](orbit_interpolation.md)，准备时捕获资源，首次查询时生成所需完整滑行段；地面站使用 WGS84 固定 ITRF 模型。当前 `state()` / `states()` 接受卫星和地面站；需要传感器位置时，调用方应传入其父卫星引用，直接传传感器会报能力错误。GCRF 是 Python 查询的显式默认值，SGP4 原生输出为 TEME，地面站原生输出为 ITRF；跨参考系查询必须准备 EOP 数据，HPOP 的非中心引力在 GCRF 查询中也依赖 EOP，绝不把 TEME 当成 GCRF。

| 参数或行为 | 约定 |
| --- | --- |
| `at=Epoch(...)` | 单历元，返回一个 `CartesianState` |
| `step=u.s(30)` | 规则采样，包含起点和精确终点；不整除时追加较短的最后一步 |
| `first=...`、`last=...` | `step` 查询的可选子区间，可以为同一历元 |
| `times=epochs` | 与 `step` 互斥；空序列返回有完整类型信息的空结果 |
| `u.s()`、`u.ms()`、`u.ns()` | 转成精确的 int64 纳秒；拒绝亚纳秒精度、非有限值和溢出，查询步长必须为正 |
| 查询范围 | 全部历元必须位于准备区间，不越界传播 |
| 单次状态序列 | 最多 200000 个样本，分配前检查；更长序列拆分查询 |
| 对象范围 | 同一项目捕获的对象；拒绝其他项目同 ID 句柄和准备后新增的对象 |

时间零点仍为 **2000-01-01 12:00:00 TAI**，不是 J2000 TT。采样在整数纳秒上进行，避免大历元与小步长叠加丢精度。项目自身现有的区间有效性约束仍然适用。

## 数据与确认计划

推荐从已经安装数据的目录准备分析，Python 和 C++ 与桌面共用完整覆盖优先、来源优先级和有限外推的选择规则：

```python
catalog = of.DataCatalog("local/shared-data")
analysis = project.prepare(data=catalog)
trajectory = analysis.states(sat, step=u.s(30), frame=of.FrameId.ITRF)

# 可选：显式修改这个目录的共享策略，之后的准备才会使用。
catalog.preferred_eop = of.EopProduct.C04
catalog.set_extrapolation_days(of.EopProduct.C04, 3)  # 0 关闭；最多 30 天

# 也可以提前取得固定资源，传给一个或多个分析。
data = catalog.snapshot(project.first, project.last)
analysis = project.prepare(data=data)
```

目录路径由调用方显式提供；选源不联网、不读取其他目录、不更改当前文件指针。默认 C04 优先；先按优先级找完整覆盖整个区间的当前来源，全部不足时再考虑各来源允许的外推，不拼接两份文件。原始数据已经覆盖时不会启用外推，因此未使用的外推设置不改变计算哈希。`catalog.snapshot()` 允许单历元区间，反向区间报错；分析项目仍遵守自身的时段约束。

选源在一次 SQLite 读取中捕获当前 UTC、EOP 指针、优先级、外推策略和旧目录兼容索引，然后校验并完整加载资源。其他进程发布新数据、目录对象释放或文件随后移除，都不会改变已有快照或确认计划。当前 UTC 缺失、损坏或失效时明确报错；仅目录尚未选择 UTC 时使用内置表。当前 EOP 文件失效时可选择另一个可用来源，并通过 `data.selection_notes` 说明；有显式当前指针时不会回退到该产品的旧缓存。没有当前指针的旧目录按有效获取记录 / 插入顺序选择每种产品的当前候选。覆盖不足的错误包含所需 TAI 纳秒区间、候选原始范围、外推天数及失效原因。

低层 `install_eop()` / `import_c04()` / `import_finals()` 只安装资源。需要从脚本指定刚安装的文件时，使用 `catalog.activate_eop(entry.lock_sha256)`；它检查该文件的 UTC 依赖后更新相应当前指针，不改下载记录和正在运行的分析。桌面下载与导入继续通过原有校验和发布流程管理当前文件。`preferred_eop` 与 `set_extrapolation_days()` 使用数据中心的同一组目录设置。

无外部时间依赖的原生状态查询不需要 EOP、联网或图形资源。SGP4 显式选择时间表：

```python
analysis = project.prepare(data=of.DataSnapshot.builtin_time())
# 只需要目录当前时间表时，不要求安装 EOP：
data = catalog.snapshot(project.first, project.last, earth_orientation=False)
analysis = project.prepare(data=data)
```

参考系转换显式加载本机文件：

```python
data = of.DataSnapshot.load(
    eop_manifest="data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json",
    # 可选；未指定时，此次显式加载使用程序内置闰秒表。
    time_file="data/samples/time/iers_leap_seconds_2026_07.dat",
    extrapolation_days=0,
)
analysis = project.prepare(data=data)
```

`DataSnapshot.load(time_file=...)` 也可只加载时间表。文件在加载时完成解析与校验，计算不重新打开文件。`data.time_info`、`eop_info` 和 `data_lock` 提供所选资源的覆盖、策略和身份。这个类型表示已选择的运行时资源，不增加历史版本界面。

`project.plan_prepare(data=data)` 固定对象输入、修订、数据和传播会话，先检查资源完整性与整个准备区间的覆盖。有限 EOP 外推为 0–30 天，0 表示关闭；它不延长 UTC 表有效期。无法覆盖或时间失效时立即抛出相应错误，不能通过同意更新绕过。

所选数据与项目记录的相同角色比较，身份或解释策略不同则列入 `plan.data_changes`。新项目没有相应记录时可直接准备；未选用的旧 EOP 不妨碍纯惯性轨道查询。直接 `project.prepare()` 遇到变化时抛出 `of.DataChangedError`，其 `plan` 是本次已固定的计划。

```python
try:
    analysis = project.prepare(data=data)
except of.DataChangedError as error:
    plan = error.plan
    # 应用展示 plan.data_changes，并取得本次明确决定。
    # 用户同意后调用；拒绝则不继续，保留已有分析。
    analysis = plan.prepare(accept_data_changes=True)
```

上例中的接受调用应放在应用的确认分支内。库本身不弹窗、不联网、不替调用方同意更新。接受仅影响这次调用；计划不会记住“以后都同意”。如果项目修订已变化，计划抛出 `RevisionConflictError`，需要重新生成。Python 编辑上下文内不能准备分析；已生成的旧分析仍可查询。

`plan_prepare(data=catalog)` 与直接传入快照使用同一确认流程，固定选源时对应的项目修订。准备成功不修改项目来源记录，不把计算结果写回项目。

## 保存与重新打开

```python
analysis = project.prepare(data=data)
project.save("mission.orbitforge", analysis=analysis)

# 相同数据直接打开；数据变化时由应用取得确认。
try:
    restored = of.Project.open("mission.orbitforge", data=data)
except of.DataChangedError as error:
    plan = error.plan
    # 展示 plan.data_changes；仅在用户明确同意的分支执行：
    restored = plan.open(accept_data_changes=True)
```

`save(..., analysis=analysis)` 保存当前已提交输入、样式和报告定义，同时记录该分析固定的 UTC / EOP 来源与策略、整个项目的物理输入哈希和数据锁哈希。它不保存结果缓存，也不声称已经执行过所有查询。分析必须属于这个项目，且物理输入仍一致；允许改名、调整样式、播放时间和报告配置；仅用于显示的轨道步长也可改变。启用 HPOP 固定星历输出后，轨道步长属于物理计算输入。修改轨道、分析区间、站点坐标或最低仰角、传感器视场或引用关系后，保存旧分析抛出 `RevisionConflictError`。同稳定 ID 的另一个项目也不能借用该分析。

`project.analysis_record` 返回只读的准备记录；`project.analysis_status` 为 `UNRECORDED`、`CURRENT` 或 `STALE`，由记录与当前输入、数据身份比较得出。输入哈希使用独立的 `orbitforge-analysis-input-v1` 编码，不等同于单对象轨迹的输入哈希。裸 `save(path)` 只保存现状，不创造准备记录，也不把过期记录改成当前；单纯 `prepare()` 仍不改写项目。更改传播模型的解释规则时须同步升级这份哈希编码的版本。

文件和文档共同遵守原子发布：候选校验、项目身份与修订检查通过后写入文件，成功后才更新内存来源、修订和已保存标记并通知观察者。覆盖默认关闭。写入失败或修订冲突不修改两者；观察者在保存成功后再次编辑，项目仍正确显示未保存。

开发预览阶段只读写当前 `.orbitforge` v40；所有打开入口（包括 `load_project_data()`）在解析内容前拒绝非当前版本，并给出实际与支持的版本。没有旧格式迁移或缺字段补默认值的读取路径；输入项目仍可以不包含可选的分析准备记录。详见[项目文件](project_files.md)。

`Project.plan_open(path, data=...)` 固定解析后的文件和已验证资源；`name`、`data`、`data_changes` 可供确认界面读取。直接 `Project.open()` 和 `of.load_project()` 使用同一流程。已有来源依赖必须提供相应当前数据；不传数据只适合没有 UTC / EOP 依赖的原生坐标计算项目，SGP4 仍须显式提供 UTC。需要仅检查或导入原始输入而不准备计算时，使用 `load_project_data()`，再按需 `Project.from_data()`。

文件、目录或当前数据指针在确认期间变化，不影响该计划；每次 `plan.open()` 返回独立项目，接受决定不保存为后续自动同意。缺失依赖、覆盖不足和 UTC 失效在创建计划时即报错，不能用确认参数绕过。数据相同时新打开的项目不标记未保存；接受不同来源后更新来源并标记未保存，旧准备记录按哈希判断是否过期。打开不自动写回原文件，也不自动刷新旧准备记录；以新分析显式保存后才更新它。桌面接入新计划仍属后续工作。

## 结果与导出

`Trajectory` 独立持有结果，分析或项目释放后仍有效。名称和修订来自准备时的快照。`input_sha256` 记录对应对象状态输入及分析区间，排除名称、样式、无关对象及地面站最低仰角；`data_sha256` 仅记录该状态查询实际使用的依赖。未转换的二体/J2/J4、未启用第三体或光压的 HPOP 纯点质量 GCRF 状态和固定站点 ITRF 状态使用空数据锁，SGP4 TEME 依赖时间表，HPOP 非中心引力及跨参考系状态依赖时间与 EOP。HPOP 的 EOP 必须同时覆盖初始历元与分析时段；启用第三体引力或太阳光压时，无论中心引力阶次多少，都记录实际星历身份，见[光压契约](solar_radiation_pressure.md)。

```python
len(trajectory)                              # 样本数
trajectory.position.values                  # SI 三维向量元组
trajectory.epoch.tai_nanoseconds             # 精确整数元组
xyz = trajectory.position.to_numpy(unit="km")
velocity = trajectory.velocity.to_numpy(unit="km/s")
table = trajectory.to_pandas()
```

NumPy 数值列始终为 `(N, 3)`，包括 `(0, 3)` 和 `(1, 3)`；时间列为 int64 TAI 纳秒。pandas 保留整数 `epoch_tai_ns` 列和 SI 数值列，将单位、参考系、原点及来源放在 `attrs`。这些转换返回副本；不创建 `datetime64` 时间索引、不静默降精度。NumPy 和 pandas 均为可选依赖，缺少时仅相应转换方法报 `ImportError`。

CSV 使用精确的整数 `epoch_tai_ns`、标明 SI 单位的状态列以及原点和参考系列，复用现有报告格式化与原子发布实现，不重新计算。默认拒绝覆盖；明确使用 `overwrite=True` 才替换已有目标。空结果仍写完整表头。

## 可见区间查询

```python
analysis = project.prepare(data=data)  # DataSnapshot 或 DataCatalog，需覆盖区间的 UTC/EOP
windows = analysis.access(station, sensor, search_step=u.s(1))
# 第二个对象也可直接传卫星；传感器还会施加圆锥视场和指向约束。
for window in windows:
    print(window.start, window.end, window.duration.nanoseconds / 1e9)
windows.to_csv("access.csv")
# windows.to_pandas()                 # 可选 pandas，返回独立副本
```

`AccessWindows` 支持遍历、`len()`、正负下标及切片（返回元组）；每个 `AccessWindow` 提供 `start`、`end`、精确 `Duration` 和 `start_clipped` / `end_clipped`。截断标记表示窗口在查询边界仍可见，而非已求得实际进入/离开时刻。孤立的零时长接触不形成窗口；`first == last` 返回带完整查询信息的空集合。

查询复用准备时已创建的父卫星传播器、UTC/EOP 和参考系转换，统一在 ITRF 中判断 WGS84 椭球遮挡、地面站最低仰角、传感器视场及指向。支持现有二体、J2、J4、SGP4、HPOP；指向仍为地心下视或理想瞬时跟踪，不增加地形、折射、光行时、姿态或转动速率模型。与原有 Access/报告共享几何实现。

| 参数 | 约定 |
| --- | --- |
| `search_step` | 必填的正 `Duration`，固定发现网格含精确终点，最多 200000 个网格点 |
| `boundary_tolerance` | 默认 `u.ms(1)`，必须为正且不超过 `search_step`，用于二分细化已发现的边界 |
| `first` / `last` | 可选子区间，默认整个分析区间；必须有序并位于准备区间内 |
| `cancelled` | 可选无参数、返回 `bool` 的函数，例如 `threading.Event().is_set`；查询开始、每次求值（含边界细化）及返回前检查 |

短于发现步长的窗口或间隙仍可能遗漏；减小边界容限不能弥补遗漏。返回的非截断边界在可见一侧，距离发现的转换不超过细化容限；这项容限不是轨道或物理模型的准确度保证。取消抛出 `AnalysisCancelled`，回调异常原样传播，均不返回部分结果。计算阶段释放 Python GIL，只在调用 Python 取消函数时重新获取；这是同步查询，不是后台任务调度器。

对象归属、类型和存在性从固定快照检查。原对象删除后旧句柄仍可用于旧分析；跨项目句柄和准备后新增的对象被拒绝。缺少 UTC/EOP 抛出 `AnalysisCapabilityError`，不会隐式联网补齐。`AccessWindows` 独立持有窗口及来源，项目、分析和原始文件释放后仍可读取或导出。

`first`、`last`、`search_step`、`boundary_tolerance` 记录实际查询参数。`provenance` 包含站点/目标 ID 和捕获名称、输入修订、输入/数据哈希及算法标识；`input_sha256`、`data_sha256` 也可直接读取。输入哈希采用现有物理输入编码，仅纳入分析区间、被查询站点及其最低仰角、目标卫星、目标传感器与其跟踪站位置。其他对象、名称、样式、非活动轨道草稿及另一个跟踪站的最低仰角不参与；查询子区间、步长和容限另由上述参数记录。数据哈希包含实际选定的 UTC/EOP 与有效策略。

CSV 列为 `station_id,target_id,start_tai_ns,end_tai_ns,duration_ns,duration_s,start_clipped,end_clipped`。历元原点为 `2000-01-01 12:00:00 TAI`，纳秒列为精确整数，秒列为浮点便利值，截断列为 0/1。空结果保留表头，默认拒绝覆盖并复用报告的原子写入。pandas 使用同名列，纳秒列为 `int64`、截断列为 `bool`，查询与来源元数据保存在 `attrs`。

## AER 观测序列

```python
aer = analysis.aer(station, sensor, step=u.s(10))
# 第二个对象可直接传卫星；指定时刻也支持乱序、重复项及空集合。
selected = analysis.aer(station, sat, times=[analysis.last, analysis.first])
print(aer[0].range_m, aer[0].range_rate_mps, aer[0].visible)
aer.to_csv("aer.csv")
elevation_deg = aer.elevation.to_numpy(unit="deg")  # 可选 numpy
table = aer.to_pandas()                           # 可选 pandas
```

`AerSeries` 独立保存每个请求历元的观测，不按可见性过滤。步长、子区间、精确终点、显式时刻顺序、最多 200000 个样本及对象范围与 `states()` 一致。`first == last` 返回一个样本；空 `times` 返回带来源信息的空序列。支持遍历、正负下标、切片和取消回调。

几何基于站点 WGS84 本地东、北、天坐标：方位角从北向东为正，仰角相对当地水平面，距离及距离变化率来自包含地球自转速度转换的 ITRF 状态。`line_of_sight` 只表示椭球遮挡检查；`visible` 还包含站点最低仰角和传感器视场。查询需准备 UTC/EOP，与 `access()` 共用观测计算及相关输入哈希，不含大气折射、地形或光行时。

| 访问方式 | 内容 |
| --- | --- |
| `aer.epoch.tai_nanoseconds` | 精确 TAI 整数纳秒元组 |
| `aer.azimuth`、`elevation`、`off_boresight` | `ScalarColumn`，原始单位 rad；NumPy 可转 deg |
| `aer.range`、`range_rate` | `ScalarColumn`，原始单位 m、m/s；NumPy 可转 km、km/s |
| `column.values` | 数值或 `None` 的元组，NumPy 副本形状始终为 `(N,)` |
| `aer.line_of_sight`、`visible` | 布尔元组 |
| `aer[i]` | `AerSample`，提供 `epoch`、带 `_rad` / `_m` / `_mps` 后缀的 SI 标量和两个布尔值 |

天顶/天底方位角未定义时为 `None`；卫星目标没有离轴角，同样为 `None`。CSV 保留空单元格，NumPy/pandas 使用 NaN，不填零。CSV 包含站点/目标 ID、精确 `epoch_tai_ns`、SI 列和 0/1 标记，默认拒绝覆盖。pandas 中历元为 int64、数值为 float64、标记为 bool，`attrs` 保留坐标约定、名称、修订和哈希。转换与导出均不重新计算。

## 从同一分析生成报告

```python
state_report = analysis.report(
    of.ReportKind.CARTESIAN_STATE, objects=[sat], step=u.s(30),
    frame=of.FrameId.GCRF, units=of.ReportUnits.ENGINEERING,
    time_scale=of.ReportTimeScale.TAI,
)
aer_report = analysis.report(of.ReportKind.AER, station=station, target=sensor, step=u.s(10))
summary = analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=sat)
state_report.export_csv("states-report.csv")
```

支持现有九类内置报告：场景摘要、对象摘要、地面站、笛卡尔状态、经典轨道根数、地理坐标、轨道运动、可见区间和 AER。前三类无需步长；其余显式提供 `step`，可用 `first` / `last` 限定子区间。派生报告使用各自的物理参考系，与桌面规则一致。

Python 直接接收对象句柄，不需要构造 `ReportDefinition` 或填写 ID、名称。普通报告必须显式指定单个对象、对象集合或 `of.reports.ALL`；空集合报错，不扩展为全部对象；场景摘要不接受对象筛选。Access/AER 使用明确的 `station` / `target` 参数。选择按分析快照验证，当前已删除的旧对象仍可查询，其他项目或准备后新增的对象报错。

返回现有 `ReportResult`，可读取 `table`、`column_title()`、`numeric_value()`、`cell_text()`，并沿用原子 CSV 导出。默认工程显示单位（km、km/s、deg）及 TAI；指定 UTC 必须在准备时提供 UTC 数据，不会隐式读取当前目录。表内数据仍为 SI，显示转换只影响格式化接口。报告保存 `input_revision` 和数据身份；输入哈希沿用报告语义，包含捕获的项目及对象名称，与 `AerSeries` / `AccessWindows` 仅相关物理输入的哈希用途不同。

`cancelled=callable` 必须返回 bool，取消抛出 `AnalysisCancelled`；`progress(done, total)` 提供同步进度，两者均可省略。即使在完成进度回调中取消，也不返回结果；回调异常传回调用方。生成时固定报告配置，回调修改原配置或当前项目不影响本次计算。报告查询不重建传播器、不重新读取数据文件，也不写入项目的已保存报告配置。

报告查询与显示已拆为 `ReportQuery` / `ReportTemplate`。Python 可传 `template=of.reports.Template([...])`，或生成后用 `template.apply(result)` 重排列、切换单位和时间系统，不重新计算。C++ 支持 `Analysis::report(query, format)` 和 `ReportResult::with_template(format)`。原始 SI 表始终保留，可恢复被隐藏的列；逐列单位优先于默认单位。详见[报告](reports.md)。项目当前为 v40（RPTS v2），保存显式选择模式及显示模板；不增加旧格式兼容路径。

## C++ 入口与验证

```cpp
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
using namespace std::chrono_literals;

// document 为 application::ProjectDocument，satellite 是其 ObjectRef。
auto analysis = orbitforge::runtime::prepare(document);
auto trajectory = analysis.states(satellite, 30s, orbitforge::core::FrameId::gcrf);
orbitforge::runtime::export_trajectory_csv(trajectory, "trajectory.csv");
orbitforge::runtime::save_project(document, "mission.orbitforge", analysis);
auto restored = orbitforge::runtime::open_project("mission.orbitforge");

orbitforge::runtime::DataCatalog catalog{"local/shared-data"};
auto earth_fixed = orbitforge::runtime::prepare(document, catalog);
auto state = earth_fixed.state(satellite, document.snapshot().project->first,
                               orbitforge::core::FrameId::itrf);
auto windows = earth_fixed.access(station, sensor, {.search_step = 1s});
for (const auto &window : windows.windows()) {
    auto exact_duration = window.duration();  // std::chrono::nanoseconds
}
orbitforge::runtime::export_access_csv(windows, "access.csv");

auto aer = earth_fixed.aer(station, sensor, 10s);
orbitforge::runtime::export_aer_csv(aer, "aer.csv");
orbitforge::application::ReportQuery query;
query.kind = orbitforge::application::ReportKind::aer;
query.object_ids = {station.id(), sensor.id()};
query.step_nanoseconds = 10'000'000'000;
orbitforge::application::ReportTemplate format;
format.time_scale = orbitforge::application::ReportTimeScale::tai;
format.columns = {{"time.epoch", "TAI", ""}, {"aer.range", "km", "Range"}};
auto report = earth_fixed.report(query, format);
orbitforge::runtime::export_report_csv(report, "aer-report.csv");
```

Runtime 位于 Application 之上，分析不让对象模型反向依赖 Provider。`ProjectSnapshot` 保留进程内项目范围；`snapshot.read(ref)` 从该修订读取对象，原对象删除后仍有效。C++ 的 `AnalysisPlan` 和 `DataChangedError::plan()` 与 Python 确认行为一致。

C++ 打开使用 `ProjectOpenPlan(path, data)` / `runtime::open_project(path, data)`；`OpenDataChangedError::plan()` 返回对应打开计划，确认后调用 `plan.open(true)`。纯输入保存使用 `document.save(path)`。所有文件校验、数据确认及保存规则由共享 C++ 实现，Python 仅包装入口和异常计划。

分析构建和查询可在 Python 释放 GIL 后运行，回调期间重新持有 GIL。当前提供同步状态、可见区间、AER 和内置报告查询；Access、AER、报告支持取消回调，已提供字段选择与可复用显示模板；尚未提供通用任务系统、桌面自由选列编辑器或完整的 Analysis 桌面接入。后续按[整体设计](object_api_design.md)逐批迁移。

验证包括 C++ `Analysis.*`、Python `test_analysis.py`、现有传播/时间/哈希回归，以及安装后的 SDK 消费端。覆盖文件移除后的资源存活、同 ID 跨项目拒绝、旧结果存活、确认计划过期、有限外推、UTC 失效、整数极限附近采样、CSV 覆盖保护及可选数组适配。可运行示例见 [analysis.py](../../examples/python/analysis.py)。

目录选源由 C++ `CatalogAnalysis.*`、Python `test_catalog_analysis.py` 和桌面数据中心回归共同验证：覆盖优先级、外推上限加 1 ns、损坏的当前文件、UTC/EOP 跨进程原子更新、确认期间切换数据及安装后的 SDK 入口。示例可使用 `analysis.py --data <目录>` 验证 ITRF 输出。

Access 由 `AnalysisAccess.*`、`AccessResults.*`、Python `test_analysis_access.py` 和安装后的 SDK 消费端验证；共享几何继续通过 `Access.*` 的解析圆轨道地平线/圆锥窗口案例。覆盖报告结果一致性、跟踪目标依赖、边界截断、步长上限、取消、原文件移除、对象归属、CSV 与可选 pandas。可运行示例为 [access.py](../../examples/python/access.py)。当前没有新增 STK 同输入数值对比数据，不据此声明与 STK 精度等价。

AER 与统一报告还由 `test_analysis_aer_reports.py` 和 `AnalysisAccess.*` 验证：九类报告与原入口数值一致、ITRF 观测及距离数值差分、精确采样、不可见样本、对象删除与源文件移除、完成回调取消、明确对象选择、可选数组转换和已安装 SDK。`access.py` 同时演示 AER 和报告入口。

报告模板由 `Reports.*`、`ReportDialog.TemplateUnitsReuseResultsAndReorderedFieldsKeepPlotsAndSelection`、Python `test_report_templates.py` 和安装后的 SDK 验证；覆盖逐列单位、重排与恢复字段、显式全选、UTC 按需校验、失效输入不自动重算，以及 v40 独立二进制样本精确往返。

### 机动记录

任务序列卫星通过 `analysis.maneuvers(satellite, cancelled=...)` 返回拥有自身数据的 `ManeuverSeries`。`events` 包含段索引、名称、GCRF 机动前后状态、GCRF Δv 与累计 Δv；结果附对象、修订、算法及输入/数据哈希。非任务序列卫星返回空列表；不接受地面站或传感器。详见[任务序列](missions.md)。

### 本体星历导入

```python
catalog = of.DataCatalog("local/shared-data")
catalog.install_de440s("local/de440s.bsp")
entry = catalog.install_planetary_centres("local/mar099s.bsp")
# 也可导入已有完整快照：
# entry = catalog.install_ephemeris_snapshot("local/ephemeris/manifest.json")
data = of.DataSnapshot.builtin_time().with_ephemeris(
    "de440s", entry.manifest, "local/spice-runtime")
```

两种导入与 C++ `DataCatalog` 共用实现；计算前仍须选择 SRP 遮挡体，并验证完整传播区间的覆盖。详见[数据中心](data_center.md#太阳系星历与日月引力)。
