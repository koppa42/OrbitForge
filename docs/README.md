# 文档导航

首次使用请从[构建与测试](development/build.md)和[场景工作台](architecture/scenario_workbench.md)开始。功能概览见[项目首页](../README.md)，数值容限和测量范围见[验证方法](validation/README.md)。

## 当前设计

| 主题 | 文档 |
| --- | --- |
| 场景与工作区 | [场景与属性工作流](architecture/scenario_workbench.md)、[对象与数据管理](architecture/object_data_management.md) |
| 对象 API | [Python 对象句柄、公共 C++ 事务与迁移](architecture/object_editing.md) |
| C++ 项目 API | [聚合头文件、对象所有权、数据确认与安装后链接](architecture/cpp_api.md) |
| 分析 API | [固定输入、数据确认、状态查询与导出](architecture/analysis_api.md) |
| 轨道模型 | [预报器](architecture/propagators.md)、[J2](architecture/j2.md)、[固体潮](architecture/solid_tides.md)、[海潮](architecture/ocean_tides.md)、[二体验证方法](validation/p1_two_body_protocol.md) |
| 机动规划 | [任务序列与脉冲机动](architecture/missions.md)、[验证协议](validation/mission_protocol.md) |
| 地面站 | [WGS84 地面站与对象生命周期](architecture/ground_stations.md) |
| 可见性与传感器 | [Access、AER、子传感器、STK 对照与验证](architecture/access_sensors.md) |
| 项目与数据引用 | [项目文件格式](architecture/project_files.md)、[场景数据一致性](architecture/scenario_data_lock.md) |
| 数据中心 | [数据类型、下载更新、来源优先级与外推](architecture/data_center.md) |
| 报告与曲线 | [数据接口、桌面交互、STK 对照及后续阶段](architecture/reports.md) |
| 时间与参考系 | [时间尺度、EOP 与状态变换](architecture/time_frames.md) |
| 星历 | [CSPICE 内核、时间与并发约定](architecture/spice.md) |
| 图形 | [二维 / 三维集成、渲染与资源生命周期](architecture/graphics.md) |
| 模块边界 | [基础类型与接口契约](architecture/p0_contracts.md) |

## 重构路线

- [统一对象 API 与数据驱动设计](architecture/object_api_design.md)：Python / C++ 接口、对象事务、分析快照、界面联动与迁移路线；对象编辑阶段已实现，计算与桌面适配按阶段推进。
- [HPOP 与 STK 的对照及重构要求](architecture/hpop_alignment.md)：完整力模型、数据依赖、积分、协方差、桌面分组与分阶段数值验收；目标能力与 0.2.1 现状分开说明。

## 开发与验证

- [开发者规范：模块职责、实现约定、验证与 PR 要求](development/requirements.md)
- [构建、测试、SDK / Python 包及便携交付](development/build.md)
- [CI 与版本发布](development/ci.md)
- [数值、计算吞吐量与图形验证方法](validation/README.md)

仓库保留当前接口约定、验证方法与必要的固定输入。开发过程记录、逐次日志、截图及程序包保存在本地，不随源码分发；复现时按对应文档重新生成。
