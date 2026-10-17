# 验证方法

本目录保存验证范围、固定输入、数值容限和测量方法。构建与测试命令见[构建说明](../development/build.md)，脚本位于 [tools/validation](../../tools/validation/)；必要的测试夹具和运行资源分别保存在 `tests/`、`data/` 及桌面资源目录。

| 主题 | 方法 |
| --- | --- |
| EOP 与完整状态变换 | [数据覆盖、导数与独立交叉校验](p0_eop_state_protocol.md) |
| CSPICE | [时间、几何星历与内核生命周期](p0_spice_protocol.md) |
| 通用二体 | [解析性质、边界与误差](p1_two_body_protocol.md) |
| HPOP | [静态球谐重力、独立数值参考、API 与 UI](hpop_protocol.md) |
| HPOP 光压局部偏导 | [Orekit、高精度两圆面积与 SciPy 多体并集](srp_differential_protocol.md) |
| HPOP 阻力偏导与密度稳定性 | [Orekit 自动微分、GMAT 原方程高精度与低空独立模型](drag_differential_protocol.md) |
| HPOP 其他力状态偏导 | [第三体及三项相对论的独立自动微分](force_differential_protocol.md) |
| HPOP 引力空间偏导 | [解析 Hessian、全阶 Orekit 和高精度极轴对照](gravity_differential_protocol.md) |
| HPOP 时间正则化内核 | [Sundman 方程、独立高精度参考及步数收敛](time_regularization_protocol.md) |
| 计算吞吐量 | [固定工作负载与计时边界](p0_compute_protocol.md) |
| 图形与性能 | [窗口生命周期、固定万星负载与呈现测量](p0_graphics_protocol.md) |

基础时间与旋转矩阵的固定参考值说明见 [tests/reference/time_frames](../../tests/reference/time_frames/README.md)，J2 长期项的模型与验证边界见 [J2 接口约定](../architecture/j2.md)。测试覆盖的模型和输入范围有明确边界，不将容限等同于真实轨道或地球定向的绝对精度。

每次执行应记录提交版本、依赖版本、环境、输入哈希、原始结果和失败原因。图形集成预检、计时工具接入与正式性能验收分别判断，不能由某项测试成功推定全部通过。

日志、截图、JSON/CSV/XML 报告及环境清单写入新的 `out/validation/<运行名称>/`；便携包、wheel 和交付目录放在 `out/releases/` 或 `out/dist/`。这些目录由 Git 忽略，不将逐次结果、开发过程记录或源码副本放进文档目录。
