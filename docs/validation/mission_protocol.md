# 任务序列验证

目标是验证按序继承状态、脉冲方向和数值传播；不声称与完整 STK Astrogator 达到同等精度。模型约定与官方参考见[任务序列](../architecture/missions.md)。尚无相同初态、μ、力模型、积分参数、历元和参考系的 STK 导出数据直接对照。

## 固定物理案例

`core.Mission.HpopPointMassHohmannTransferMatchesAnalyticApoapsisAndCircularization`：圆轨道 7000 km → 8000 km，μ=`3.986004415e14 m³/s²`。传递椭圆 a=`7500 km`，飞行时间 `π√(a³/μ)` 四舍五入到整纳秒。两次 VNC 顺行脉冲分别使用活力公式与圆轨道速度之差。HPOP 为 0/0 点质量、默认 RKF78，无外部数据。

到达远地点后的状态与解析 `r=(-8000 km,0,0)`、圆化后的负 Y 向速度比较：位置每分量 0.02 m，速度 2e-5 m/s。继续传播 3600 s 后半径误差不超过 0.05 m。容限覆盖默认积分局部误差积累与纳秒时间舍入，不代表实际轨道预报精度。Python `examples/python/mission.py` 对独立 CSPICE 二体模型执行同一转移，最终半径误差小于 1 mm。

## 行为回归

- `core.Mission.*`：线性测试 Provider 的精确分段解；初始/最终时刻及同刻连续脉冲；任意顺序和并发查询；取消后重试；无效方向、时长、区间及溢出。非圆轨道用显式基向量检查 VNC 与 STK LVLH 的方向，避免仅用圆轨道掩盖定义差别。
- `integration.MissionIntegration.*`：项目精确读写、覆盖不足、EOP 依赖、机动名称不改变计算哈希、物理参数改变哈希、结果来源和旧分析不可变。
- `desktop.OrbitPreparation.MissionBurnsKeepSeparateTangentsOnAndOffTheDisplayGrid`：网格内/外的点火时刻，前一纳秒与点火时刻都保留为节点，节点速度来自真实传播，GCRF/ITRF 插值检查维持小于 1 m。
- `desktop.QuantityTest.Mission*`：增加、排序、非法输入、区间不足、取消及重开；纳秒点火时间与后续时间同步更新；无效输入恢复为原数值时清除错误；窄页切换与尺寸变化保留单位和未完成输入；空状态、保存时定位错误字段，以及列表快捷键不误保存。设置 `ORBITFORGE_MISSION_UI_CAPTURE` 可保存宽、窄、400×520 短窗口、长名称和错误状态；另以 `QT_SCALE_FACTOR=1.5` 检查高 DPI。
- Python `test_mission.py`：简洁构造器、不可变配置、机动结果、报告共用、文件往返、错误不修改项目、取消和跨项目引用拒绝。独立虚拟环境安装已编译扩展后执行。
- C++ SDK 消费端 `tests/packaging/sdk_consumer/project_api.cpp`：通过已安装公共头文件创建 HPOP 点质量任务、查询脉冲、保存并打开。关闭 ERFA/CSPICE 的 SDK 也应支持此模式。
- `project-current.orbitforge` 独立样本：v36 / OBJS v31，包含 HPOP 滑行和三轴 VNC 脉冲、非整秒纳秒时长；C++ 与 Python 必须精确重写原始字节。所有旧版本及未来版本明确拒绝。

## 执行与证据

完成依赖配置后执行 `cmake --build --preset desktop-graphics`、`ctest --preset desktop-graphics`。聚焦调试可用 CTest 正则 `Mission|CurrentSchema`。Python 执行完整 `unittest discover -s bindings/python/tests -v` 与新示例；SDK 按[构建说明](../development/build.md)安装后独立编译消费端。

日志与截图保存在 `out/validation/<run>/`，记录平台、构建配置、命令和结果。离屏 UI 与数值缓存测试不等价于真实 Vulkan 性能验收；本次不修改 GPU 或原生窗口生命周期。
