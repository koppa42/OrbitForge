# 地球 HPOP 第三体引力

HPOP 的“力模型 → 引力”页中，第三体区域按参考图排列 Name / Use / Source / Gravity Value：Sun、Moon、Jupiter、Venus、Saturn、Mars、Mercury、Uranus、Neptune、Pluto。十个天体独立启用，搜索只筛选显示；关闭的行仍保存来源与有效草稿，不参与计算身份。所有积分阶段使用该阶段历元的地心几何位置，并通过稳定差分公式减去地心本身受到的引力。

| 来源 | GM 的实际含义 |
| --- | --- |
| JPL DE（当前默认） | 与选定 DE440s 配套的 NAIF `gm_de440.tpc` 常数，API 使用 m³/s² |
| Cb file | 显式导入对应 `Sun.cb` / `Jupiter.cb` 等文件的 Gm 或 SystemGm，记录原文 SHA-256；没有导入时不能启用此来源 |
| User Specified | 用户设置的正、有限 SI GM；桌面默认显示 km³/s²，可以切换单位 |

STK 来源语义依据[HPOP Gravity](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)，Gm / SystemGm 定义依据[Central Body Settings](https://help.agi.com/stk/Content/stk/referenceFramesSettings.htm)。当前默认 JPL DE 不等于截图中 Cb file 的默认组合。位置由数据中心管理的 DE440s 提供；不导入 Cb 中的 Analytic / File / JplSpice 星历设置，也不声称复刻 STK 自带的所有天体。STK 的实际 DE 文件版本、Cb 文件原文及中心约定需在数值比较时单独核对。

| 天体 | NAIF 目标 | 位置 / GM |
| --- | --- | --- |
| Sun / Moon | 10 / 301 | 天体中心 / 天体 GM |
| Mercury / Venus | 199 / 299 | 天体中心 / 天体 GM，DE440 中与对应系统质心重合 |
| Mars / Jupiter / Saturn / Uranus / Neptune / Pluto | 4 / 5 / 6 / 7 / 8 / 9 | 系统质心 / 系统 GM，含卫星质量 |

常数取自 [NAIF DE440 GM](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/gm_de440.tpc)，位置沿用现有 ICRF 轴适配器；不把 SPICE 状态改标签为 GCRF。全传播区间检查目标链、参考系、中心及内部空隙，缺失任一启用目标时准备失败。原有日月验证片段不能用于行星传播；完整数据中心 DE440s 与新的太阳系离线片段均具有所有必需链。

Cb 导入只读取 GM 子集：逐行 `Gm value`、`SystemGm value`（也接受 `key = value`、D 指数和 `#` 注释）。文件名必须匹配所选天体；拒绝重复字段、非有限或非正数、二进制及大于 1 MiB 的输入。单位由调用方显式指定，默认 m³/s²；km³/s² 使用比例 1e9。火星及外行星要求 SystemGm，避免把行星中心 GM 与系统质心位置混用。日月和内行星要求 Gm；除作为地球父天体的太阳使用 Gm 外，存在 SystemGm 时优先采用系统值。其他记录不会用于引力计算。

导入成功后捕获 GM、文件名、天体和原文哈希作为配置快照，关闭窗口或导入失败保留原设置；计算不读取外部文件。项目 v40 保存十条来源与草稿，重开不依赖原 Cb 路径。输入身份只包含启用天体、实际来源及所用 GM / 哈希；改变未活动草稿不使分析或报告失效。

C++ 使用 `options.third_body(core::ThirdBody::jupiter)` 设置 `enabled`、`source`、`user_mu` 或 `cb`，通过 `runtime::load_cb_gravity(path, body, mu_scale)` 捕获文件常数。

```python
cb = of.orbits.load_cb_gravity("Jupiter.cb", "jupiter", mu_scale=1.0)
orbit = of.orbits.Hpop(
    initial=initial, gravity_degree=0,
    third_bodies=("sun", "moon", "jupiter"),
    body_gravity=(of.orbits.ThirdBodySettings("jupiter", "cb_file", cb=cb),),
)
# 手动来源：ThirdBodySettings("jupiter", "user_specified", mu=1.26712764e17)
# sun_mu / moon_mu 仍是 Hpop() 中构造手动来源的便捷参数。
```

低层 Python `HpopOptions.third_bodies` 是固定长度的十行值数组；修改行后需将整个数组赋回。高层 `HpopSettings` / `ThirdBodySettings` 保持不可变。数值与失败路径见[HPOP 验证](../validation/hpop_protocol.md)。目前没有同配置 STK 导出轨迹，功能接入及独立参考通过不代表 STK 数值对齐已完成。
