# OrbitForge

航天任务分析与仿真平台，提供桌面轨道工作台、C++ SDK 和独立 Python 包，共用一套计算与场景接口。

当前版本为 **0.2.1 开发预览**，已提供轨道工作台、数据管理和基础报告，支持地面站可见性、卫星子传感器分析、HPOP 数值传播及脉冲机动序列，后续将扩展摄动力模型与覆盖分析。

[构建与测试](docs/development/build.md) · [工作台使用与设计](docs/architecture/scenario_workbench.md) · [开发者规范](docs/development/requirements.md) · [文档导航](docs/README.md) · [验证方法](docs/validation/README.md)

## 已实现

| 能力 | 当前范围 |
| --- | --- |
| 场景与对象 | 统一分析时段；多卫星、WGS84 地面站；插入、编辑、重命名、复制和删除 |
| 轨道计算 | 二体、J2/J4 长期项、SGP4/TLE、HPOP 静态重力（内置 EGM96、文件模型至 2190 阶、RKF78/RKV89/Bulirsch–Stoer/Gauss–Jackson 12/RK4）；GCRF / ITRF / TEME 状态变换 |
| 机动规划 | [任务序列](docs/architecture/missions.md)：定时滑行、GCRF/VNC/LVLH 脉冲 Δv；二体或 HPOP 滑行、机动前后状态与累计 Δv |
| 时间与星历 | UTC / TAI / TT / TDB 转换；ERFA、IERS 与 CSPICE 接口；数据中心下载完整 DE440s 太阳系星历 |
| 二维与三维 | 地面地图与地球视图共享时间和选择；昼夜显示、局部轨迹缓存、可拖动和弹出的工作区 |
| 地面站联动 | 最低仰角、可见时段、AER；卫星圆锥子传感器、对地心 / 理想跟踪；时间轴与二维地图关联 |
| 卫星显示 | 轨迹线型、颜色、粗细及即时预览；按对象保存；可选本地三维模型 |
| 数据中心 | C04、finals2000A、UTC / 闰秒完整文件下载与自动更新；来源优先级、覆盖检查、有限 EOP 外推 |
| 报告与曲线 | 九种报告（含 Access / AER）；对象范围、时间采样、多对象曲线、缩放与时刻定位、CSV / PNG 导出、项目内模板 |
| 报告模板 API | Python/C++ 共用查询与模板模型；字段选择、顺序、逐列单位与标题，复用已有结果更换显示格式 |
| 项目文件 | `.orbitforge` 版本化容器、完整性校验、原子保存；开发阶段仅读写当前 v38，不兼容旧格式，API 支持数据确认和分析来源记录 |
| 开发接口 | 模块化 C++20 库与 CMake SDK；pybind11 Python 包；桌面与脚本共用 Runtime |

新场景使用当前计算数据；打开或重新计算时，数据发生变化须经用户确认。计算过程保持数据一致，项目记录实际使用的数据哈希供溯源。外部模型和 EOP 文件仍为独立依赖，详见[项目格式](docs/architecture/project_files.md)和[数据中心](docs/architecture/data_center.md)。

图形桌面首次启动自动准备 UTC / 闰秒表、完整 C04 与 finals2000A、EGM2008，以及启用 CSPICE 时的 DE440s。准备窗口支持进度、失败重试、导入官方文件和取消退出；已完成的文件保留，后续启动只补齐缺失项。新建 HPOP 默认使用 EGM2008、21×21，文件重力计算支持至 2190 阶，EGM2008 可使用完整 2190×2159；阶数与次数按实际文件范围独立选择。

尚未实现 OMM / 在线卫星目录、通用事件分析、覆盖分析、HDF5 结果存储或通用分析任务系统。HPOP 支持地球静态引力、[永久潮／完整固体潮](docs/architecture/solid_tides.md)、[FES2014bv1 海潮与海洋极潮](docs/architecture/ocean_tides.md)、可选太阳系第三体引力、[球形大气阻力](docs/architecture/atmospheric_drag.md)、[球形太阳辐射压／球面地月阴影](docs/architecture/solar_radiation_pressure.md)及[IERS 2003 三项相对论](docs/architecture/relativity.md)，数据中心可下载 EGM2008 / EGM96 / FES2014bv1 / DE440s / CelesTrak 空间天气或导入相应文件；天气插值更新率、月预报、光压高级设置、重力长期变化、潮汐幅度筛选与次要分潮推算、附加力及协方差按[完整对齐清单](docs/architecture/hpop_alignment.md)继续推进。J2/J4 使用长期项平均根数，SGP4 使用固定 TLE（见[预报器说明](docs/architecture/propagators.md)）。时间与参考系尚无日内 EOP 潮汐改正。万星图形场景已有验证入口，尚未完成正式性能验收。

## 获取与构建

```powershell
git clone https://github.com/koppa42/OrbitForge.git
cd OrbitForge
```

需要 **CMake 3.28+** 和 **C++20 编译器**。Windows 使用 Visual Studio C++ 工具链；Linux 无界面构建已在 Ubuntu 24.04 / GCC 13.3 验证。Python 包要求 Python 3.11+，图形桌面需要 Vulkan 驱动。

### 无界面示例

基础构建只使用标准库，不需要先安装 Qt 或天文计算依赖：

```powershell
cmake --preset headless
cmake --build --preset headless
cmake --install out/build/headless --config Release
```

Windows/MSVC 示例程序为 `out/build/headless/examples/cpp/Release/orbitforge_example.exe`；单配置生成器的路径不含 `Release/`。只构建 Core 可使用 `core-only` 预设。

### 桌面工作台与完整计算接口

按[构建说明](docs/development/build.md)安装固定版本的 vcpkg 功能依赖，再选择对应入口：

| 目标 | 入口 |
| --- | --- |
| Windows 二维 / 三维工作台 | `desktop-graphics` 预设；构建后执行 `pwsh tools/dev/run-desktop.ps1` |
| ERFA / IERS / CSPICE 无界面计算 | `astronomy` 预设 |
| C++ SDK | CMake 安装目标；[统一项目 API](docs/architecture/cpp_api.md)与[可运行示例](examples/cpp/project_analysis.cpp) |
| Python 包 | 根目录 `pyproject.toml`；需要天文计算依赖及 SQLite |
| Linux 构建与交付验证 | `linux-astronomy`、`linux-sanitizers` 预设及 `tools/ci/check_linux.py` |
| Windows 便携包 | `tools/packaging/package_windows.py` |

`desktop` 预设是基础 Qt 状态表窗口；完整场景工作台使用 `desktop-graphics`。便携包、wheel 与构建缓存位于本地 `out/`，不随源码克隆分发。

Python 对象统一通过 `project.objects` 创建，支持带类型的对象句柄、属性赋值、关键字 `update()` 和 `with project.edit():` 原子编辑。用法及旧接口迁移见[对象编辑 API](docs/architecture/object_editing.md)；完整后续路线见[统一 API 与数据驱动设计](docs/architecture/object_api_design.md)。

C++ 包含 `<orbitforge/orbitforge.hpp>`，通过 `of::Project` 的 `objects()`、`prepare()`、`save()` 与 `Project::open()` 完成相同流程；创建对象使用 `of::objects::Satellite` 等配置值，事务显式提交。参见 [C++ 项目 API](docs/architecture/cpp_api.md)。

`project.prepare()` 捕获固定分析输入，提供状态、可见区间、AER 和九类内置报告查询：`analysis.states(sat, step=u.s(30))`、`analysis.access(station, sensor, search_step=u.s(1))`、`analysis.aer(station, sensor, step=u.s(10))`、`analysis.report(of.ReportKind.CARTESIAN_STATE, objects=[sat], step=u.s(30))`。可见区间与 AER 需准备 UTC/EOP 数据；数据变化通过固定计划确认。结果可直接导出 CSV，NumPy/pandas 为可选适配。参见[分析 API](docs/architecture/analysis_api.md)、[状态示例](examples/python/analysis.py)及[可见区间、AER 与报告示例](examples/python/access.py)。

报告可通过 `template=of.reports.Template([...])` 选择字段和显示单位，也可用 `template.apply(result)` 复用已有结果。全部适用对象使用 `objects=of.reports.ALL`，空列表报错。详见[报告模板](docs/architecture/reports.md)和[可运行示例](examples/python/report_templates.py)。

## 测试与验证

核心与集成测试使用 GoogleTest / CTest，Python 接口使用 unittest。时间和参考系提供 Orekit 交叉校验，星历使用 jplephem 读取原始 DE440s 对照；固定输入与复现脚本随仓库保存。

数值容限、图形检查和性能测量范围见[验证方法](docs/validation/README.md)。按[构建说明](docs/development/build.md)运行当前版本的检查；每次执行的日志、截图和报告保存在本地 `out/validation/`。

GitHub Actions 对代码 PR 运行两平台测试及 Python 版本矩阵；代码合入 `main` 后验证 C++、桌面和基准 Python，不重复其他 Python 版本。纯文档 PR 和纯文档合并只运行轻量检查。PR 和 `main` 只编译测试需要的程序与扩展，不构建发布包。推送指向已合入 `main` 提交的 `vX.Y.Z` 标签后，才构建 Windows 便携包、Windows/Linux C++ SDK 和 Python wheel，通过完整测试及安装验证后发布；版本标签禁止修改或删除。检查范围、缓存与发布方法见 [CI 与版本发布](docs/development/ci.md)。

## 参与开发

参与编码、测试和评审前，请阅读[开发者规范](docs/development/requirements.md)，了解模块职责、接口与数值计算约定、数据和后台任务处理、验证方法及 PR 要求。

**所有改动必须通过 PR，禁止直接推送 `main`**，文档、CI 和依赖更新也不例外。从最新 `main` 创建主题分支，围绕明确目标组织改动，并同步相关测试、文档和示例。推送主题分支后创建 PR；提交与推送不代表授权合并。

提交 PR 时说明解决的问题、最终行为和实际验证结果；合并前更新到最新目标分支，确认当前提交的 `CI passed` 通过，再由维护者确认合并。本地按改动范围运行必要检查，Python 日常验证使用 3.13，其他受支持版本交由 CI 验证。

## 目录

| 目录 | 内容 |
| --- | --- |
| `modules/` | Core、Results、Application、Runtime、Provider 与可视化适配层 |
| `apps/desktop/` | Qt Widgets 桌面工作台与界面测试 |
| `bindings/python/` | 原生绑定、Python 包、类型声明与 API 测试 |
| `examples/` | C++ / Python 使用示例 |
| `data/` | 必要的离线样例、资源、数据清单与来源说明 |
| `tests/`、`benchmarks/` | 集成测试、SDK 消费端与计算 / 图形基准 |
| `cmake/`、`tools/` | 构建配置、vcpkg overlay、导入、验证与打包工具 |
| `docs/` | 当前设计、开发说明与验证方法 |
| `out/`、`local/` | Git 忽略的生成产物与本机数据 |

第三方代码、字体、影像及数据的许可证或来源说明保存在对应资源目录。开发机配置使用 `CMakeUserPresets.json`；提交范围与复现要求见[构建说明](docs/development/build.md)。
