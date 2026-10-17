# 地面站

通过“插入 → 插入对象 → 地面站”创建。默认选择“定义属性”，在工作区输入名称、经度、纬度与椭球高，点击“插入地面站”生效。选择“使用默认参数”则直接创建经度 0°、纬度 0°、椭球高 0 m 的站点。属性分“站点”“位置 · WGS84”“可见性约束”三组，ITRF 坐标预览作为位置组的最后一行；支持度/弧度、m/km 换算、重置和错误提示；关闭页面不会提交草稿。

地面站与卫星共用对象树、搜索、选择、定位、重命名、复制和删除。二维显示青绿色菱形及名称，三维显示青绿色菱形；所选对象显示选中环。地固视图中位置固定，惯性视图中随地球转动。允许只有地面站的场景，也允许删除最后一个对象后继续插入。

## 坐标与计算约定

- WGS84 大地纬度，东经和北纬为正；公共 API 使用弧度、米。
- 经度范围 ±180°，纬度范围 ±90°。当前地面站模型支持椭球高 −12,000 至 100,000 m，拒绝范围外和非有限输入。
- 椭球高以 WGS84 椭球面为零，与海拔不同；不自动进行大地水准面、地形或地图拾点换算。
- 半长轴 6,378,137 m，扁率 1 / 298.257223563。约定与 [ERFA 的 WGS84 参数](https://github.com/liberfa/erfa/blob/master/src/eform.c)及[大地坐标转换接口](https://github.com/liberfa/erfa/blob/master/src/gd2gc.c)一致。
- `core::GroundStation` 是地心 ITRF 状态提供者，位置固定、ITRF 速度为零，历元保持请求的纳秒值。不含板块运动、固体潮、海潮负荷或站点实测速度。
- 转换到 GCRF 时必须使用完整 `StateTransform::inverse()`，包含地球旋转的速度项。桌面位置显示只调用位置旋转；显示缓存不能作为Access 分析的输入。

三维地球目前使用参考椭球。为使负椭球高的站点能被看到，三维标记使用 `max(椭球高, 100 m)` 的显示位置；拾取与选中环使用相同位置。每个视图独立进行椭球遮挡判断，正面的菱形与选中环作为标注叠加显示，背面的站点不绘制也不能拾取。计算状态、属性预览、二维经纬度和项目文件始终使用原始坐标。这是显示间距，不是地面站的实际高度改写。

## 场景与文件

`ProjectGroundStation` 保存稳定 ID、显示名称、`GroundStationLocation` 和最低仰角；ID 在所有对象间必须唯一。文件层 `application::Project.ground_stations`（Python `ProjectData.ground_stations`）保存站点集合；当前项目 v40 的必需 `GSTN` v2 段保存精确弧度、米值及最低仰角。开发阶段只读写当前格式，非当前版本或缺失必需段均拒绝，不从旧格式补默认值。

Runtime 构建固定 ITRF Session，模型标识为 `ground-station-wgs84-fixed-itrf-v1`。同一 Scenario 可同时包含 GCRF 卫星和 ITRF 地面站；每条 `StateSeries` 的参考系必须显式检查后再做相对位置计算。位置编辑增加该站点输入修订；重命名、场景时段编辑以及其他对象的操作保留存续数值 Session。复制使用新 ID 和修订 1，重新从文件打开也从修订 1 开始。

桌面编辑通过已有的场景候选准备流程发布。准备开始前检查当前 EOP 数据及策略；如有变化，须用户同意更新。准备使用捕获的同一 Provider，成功后一起发布树、地图、三维标记、场景及数据来源；取消或失败保留原场景。所有对象合计上限为 10,000。

Python 示例：

```python
import orbitforge as of
from orbitforge import units as u

project = of.Project("观测任务", of.Epoch(0), of.Epoch(7_200_000_000_000))
station = project.objects.ground_station("示例地面站", location=of.Wgs84Location(
    longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350)))
states = project.to_scenario().sample([project.first, project.last])[0]  # ITRF
project.save("station.orbitforge")
```

Python 普通脚本通过 `project.objects` 和站点句柄修改，例如 `station.minimum_elevation = u.deg(5)`；多对象原子修改使用 `with project.edit():`。文件层可变值保留为 `ProjectData`，详见[对象编辑 API](object_editing.md)。

已支持卫星过站、AER、最低仰角、椭球遮挡与传感器视场限制，参见[可见性与传感器](access_sensors.md)。尚无地形、方位遮挡掩模、折射和通信链路预算。

数值与文件兼容检查位于 C++ 集成测试和 Python API 测试中，图形交互及便携包检查使用[构建说明](../development/build.md)中的验证入口。
