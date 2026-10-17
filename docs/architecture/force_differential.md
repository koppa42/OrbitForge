# 第三体与相对论的状态偏导

`ForceDifferential` 保存单历元加速度及其 3×3 位置、速度 Jacobian。行是加速度分量，列是位置或速度分量；单位依次为 m/s²、s⁻²、s⁻¹。输入和输出沿用各力模型的地心惯性轴，固定力历元、外部几何星历、地球极轴、GM 和模型参数。这里计算的是对航天器状态的偏导，不包含星历、时间或参数偏导。

这些 C++ 函数是 HPOP 协方差变分方程的计算基础，随 SDK 安装。名义加速度入口保持原实现和求值顺序；新接口只在显式请求时求偏导。Python 和桌面的完整协方差入口仍待接入，不能将这些基础函数当作已经完成状态转移矩阵或协方差传播。

中心引力的全阶 Hessian 使用[独立接口](gravity_differential.md)。所有启用力项的状态偏导完成后，才能装配完整变分矩阵；[阻力的局部状态块及 D 偏导](drag_differential.md)已提供；[SRP 局部状态／太阳／K 偏导](srp_differential.md)也已提供；卫星视太阳的星历链、全局参数传播及阴影／机动／大气边界处理仍在开发范围内。

## 第三体

`third_body_differential(r, rb, mu)` 返回原有稳定的地心差分加速度，以及：

```text
d = r − rb,  s = |d|,  u = d/s
∂a/∂r = μ/s³ (3 u uᵀ − I)
∂a/∂v = 0
```

地心自身加速度项在固定历元不依赖航天器状态，其状态偏导为零。空间 Jacobian 直接解析求值，不对两个近等大的加速度作差。矩阵严格对称，零迹由测试检查；r=0 时加速度为零而位置敏感度仍非零。

## 相对论

`relativity_differential(...)` 沿用 `evaluate_relativity(...)` 的输入和 IERS 2003 约定，返回 Schwarzschild、Lense–Thirring、de Sitter 的各自 `ForceDifferential` 及总和。三项按解析公式分别求导，不能用总加速度的大项掩盖较小项的错误。

令 `A = μ/(c² |r|³)`、`F = 4μ/|r| − v·v`、`G = 4r·v`，Schwarzschild 项为 `A(F r + G v)`。使用 `∂j A/A = −3r_j/|r|²`、`∂j F = −4μr_j/|r|³` 及 `∂G/∂r_j=4v_j`、`∂G/∂v_j=4r_j` 求位置和速度块。

Lense–Thirring 项保持 `2 A J [Q(r×v) + v×p]`，其中 `Q=3(r·p)/|r|²`、地球比角动量 `J=9.8e8 m²/s`，`p` 为惯性轴中的单位地球极轴。求导包括 A、Q 及两个叉乘的全部状态依赖。

de Sitter 项保持 `B[(r_s×v_s)×v]`，`B=3μ_s/(c²|r_s|³)`；其位置块为零，速度块为这个固定叉乘算子的矩阵。零速度时，Lense–Thirring 和 de Sitter 的加速度均为零，速度偏导仍须保留；两者速度块是反对称矩阵。

沿用原模型的有限性、非奇异位置、正 GM、亚光速和单位极轴检查，并拒绝非有限偏导。结果没有共享可变状态。各力项的相对论适用域、时标和原点约定没有因求导而扩展。

独立数值依据及容限见[力状态偏导验证](../validation/force_differential_protocol.md)。
