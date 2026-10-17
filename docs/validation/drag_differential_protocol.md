# 阻力偏导与 JR 高空稳定性验证

契约见[阻力偏导](../architecture/drag_differential.md)。固定参考独立生成，普通测试不联网、不依赖 Java 或 mpmath，不从 OrbitForge 输出反推期望值。当前没有 STK 同配置实测数据。

## 独立依据

| 参考 | 范围与方法 |
| --- | --- |
| `drag_differential.txt`，672 组 | 固定 Orekit 13.1.2 / Hipparchus 4.0.2，`DragForce(..., false)` 强制用 `Gradient` 自动微分密度，七变量同时覆盖 r、v、D；检查 rho、grad rho、a、Jr、Jv、d_a/d_D。手动／日 Ap／历史 Ap、精确／近似高度、0…1000 km、接近极点、经度和太阳时切口两侧、赤道及其两侧、零／负 Cd。 |
| `drag_density_differential.txt`，72 组 | 未修改 NASA GMAT c9b78bd 标量 JR 与原作者 NRLMSISE bc9a2fe，独立笛卡尔八阶差分；95/105/115 km、南北纬和赤道、两种高度、手动／文件及混合模型。JR 和低空 NRL 使用不同天气样本。64/128 m 两组模板必须满足收敛门限。 |
| `drag_density_high.txt`，216 组 | GMAT 原方程高精度转写，系数从校验过 SHA-256 的上游源文件读取；保留原温度相减，mpmath 80/100 位计算并用 `mpmath.diff` 独立求笛卡尔梯度。140…2499 km、分段两侧、三组天气、两种高度及四组纬度；两种精度的全部结果舍入到 double 后相同。 |
| `jacchia_roberts_precise.txt`，576 组 | 与原 `jacchia_roberts.txt` 的 12 列输入逐项相同，继续覆盖季节、闰日／闰秒及 90…2500 km。≤125 km 保留未修改 GMAT 可执行程序结果，>125 km 使用上述高精度原方程。原文件仍保存，供重现与精度比较。 |

Orekit 使用固定共转轴、WGS84 和质量 1000 kg；球体面积设为 1000 m²，Cd 变量设为 D，保证参数偏导针对 STK 的 D 而非单独 Cd。合成 UTC=TAI，仅隔离时标差异；时刻为 2000-02-29 13:23:12.5，固定地心太阳 `(1.4e11,−5e10,3e10)` m。近似高度通过独立 `BodyShape` 适配输入，NRLMSISE 与 DragForce 源码均未修改。

高精度转写不是 GMAT 可执行程序，不能表述为“运行了 80 位 GMAT”。它使用同一原方程和固定系数，依赖 `mpmath` 的独立高精度运算，验证生产实现的代数改写。原始 double 参考在部分 2500 km 低太阳活动输入给出零值，高精度值约 1e−17 kg/m³，因此不能继续把该舍入失效结果当作真值；不为迁就旧结果扩大容限。

## 门限与性质

- Orekit 密度、名义加速度、速度块、D 参数块：相对 3e−11；密度梯度每分量 `1e−7 ||grad rho||∞ + 2e−13 rho/m`，位置块按相同力公式放大此界限。
- 低空独立 GMAT／NRL 梯度：`2e−7 ||grad rho||∞ + 5e−12 rho/m`。参考自身 64/128 m 收敛要求 `2e−7 ||grad rho||∞ + 2e−12 rho/m`；第二项约束原始低空多项式／求根求值经过差分放大的舍入误差，尤其密度剖面接近平坦时。名义密度相对 3e−10。
- 高精度高空密度相对 3e−11，梯度同 Orekit 门限；既有 576 组密度测试继续使用原相对门限 2e−10。
- 另检验零 Cd／面积、负 Cd、零相对速度、禁用模型、缺天气、非有限参数、有效域端点、分段和角度切口拒绝，以及惯性位置块中的地球自转速度耦合。

固定网格上的最大梯度误差除以 `||grad rho||∞`：Orekit 约 1.713e−9，高精度 JR 约 5.010e−9。低空 GMAT／混合采用 `||grad rho||∞ + rho/(10000 m)` 归一化，最大约 1.424e−8。216 组高精度 JR 密度最大相对差约 5.307e−14。以上只验证实现，不表示经验大气模型的物理精度。

## 重现

Orekit JAR 固定于 `tools/validation/orekit/dependencies.json`，生成器为 `OrbitForgeDragDifferentialReference.java`。编译后运行该类，唯一参数是候选输出文件；重复生成须字节一致。其他原始来源固定于 `tools/validation/density_sources.json`。

`drag_density_reference.cpp` 与未修改 GMAT 源码、API-only `gmat_jr_stubs` 及原作者 NRL C 文件编译为独立适配程序；不链接 OrbitForge。随后运行：

```text
python tools/validation/drag_density_differential_reference.py <adapter> <candidate-low.txt>
python tools/validation/jr_high_precision_reference.py <pinned-GMAT.cpp> <candidate-high.txt> --legacy-reference tests/reference/propagators/jacchia_roberts.txt --legacy-output <candidate-precise.txt>
```

高精度生成需 mpmath 1.3.0；生成器自行验证 80/100 位一致性及 GMAT 源字节。生成候选后审阅、核对散列，再更新固定输入；不自动覆盖参考。C++ 测试入口为 `DragDifferential.*` / `AtmosphericDrag.*`，Python 为 `test_drag.py`，安装 SDK 消费端实际调用新 C++ 接口。

| 固定文件 | SHA-256（LF 字节） |
| --- | --- |
| drag_differential.txt | `0ebbe36c805339a63d472d21d4ea6bbb1aecf3aaf5e3cea5567664473a5a074e` |
| drag_density_differential.txt | `3646675f007b2e8caf7349cae75a162cf2052d4eb71fb74c1743837dedaf61a8` |
| drag_density_high.txt | `2a409aa6af75f34a2cc0a88ab2d1a88989e0b532e136ad09ff44f933184795f0` |
| jacchia_roberts_precise.txt | `a4d9ace64d03274835a12e52fd5fbcb58c4844fc82fe10e7a59bebcb27d8faea` |
