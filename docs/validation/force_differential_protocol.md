# 第三体和相对论状态偏导验证

[接口契约](../architecture/force_differential.md)定义固定历元、地心惯性轴、位置与速度的偏导范围。被测实现采用解析公式；参考采用未修改的 **Orekit 13.1.2 / Hipparchus 4.0.2** 六维 `Gradient` 自动微分，两者不共享偏导实现。

## 输入与独立算法

生成器 `tools/validation/orekit/OrbitForgeForceDifferentialReference.java` 直接调用 `ThirdBodyAttraction`、`Relativity`、`LenseThirringRelativity` 和 `DeSitterRelativity` 的公共场接口。绝对 PV 状态使 r=0 和 v=0 也能表示，不借用奇异轨道根数。第三体几何和相对论太阳／极轴采用已有参考生成器的明确合成输入，避免真实星历、时间转换、地球定向数据差异混入力偏导对照。

- **42 组第三体**：Sun/Moon、三个固定历元、地心零位置、微米偏移、LEO、极区、GEO 和较靠近第三体的点质量几何。三维位置作为前三个独立变量，速度作为后三个变量；外部天体坐标和 GM 是常量。
- **63 组相对论**：三个太阳几何历元、三个极轴倾角、七组状态，覆盖 LEO、GEO、极区、倾斜速度、零速度、径向运动及较高速度。分别输出三项及总和，包含所有位置和速度交叉分量。

每行参考输出加速度、行优先位置块和行优先速度块。数据固定在 `tests/reference/propagators/force_differential.txt`：

```text
SHA-256 f6b047714fef426ecea2301b60978b25bcf2472d4e0898117da93d8cfc67ce31
```

八个库文件的版本、URL 和 SHA-256 由 `tools/validation/orekit/dependencies.json` 固定。先校验依赖，再编译三个生成器；`<classpath>` 使用平台要求的 JAR 分隔符，输出写入新的 `out/validation/` 目录：

```text
javac -cp <classpath> -d <classes> tools/validation/orekit/OrbitForgeThirdBodyReference.java tools/validation/orekit/OrbitForgeRelativityReference.java tools/validation/orekit/OrbitForgeForceDifferentialReference.java
java -cp <classes-and-jars> OrbitForgeForceDifferentialReference <output.txt>
```

重复运行，核对输出 LF 字节及散列。生成过程不调用生产力函数或导数。

## 门限和行为验证

令 `S` 为参考矩阵的最大绝对元素，逐分量检查：

| 项目 | 门限 |
| --- | --- |
| 第三体位置偏导 | `2e−14 S + 1e−29 s⁻²` |
| 第三体速度偏导 | 两边严格为零 |
| 各相对论位置偏导 | `4e−14 S + 1e−32 s⁻²` |
| 各相对论速度偏导 | `4e−14 S + 1e−29 s⁻¹` |

三项相对论使用各自的 S，另单独核对总和。第三体名义加速度按 `5e−13 max|a_ref| + 3e−17 m/s²` 检查，考虑 Orekit 直接相减在地心附近的浮点舍入；原有更紧的分项力测试保持不变。偏导接口返回的加速度须与现有名义入口逐位一致。

额外检查：第三体空间块的精确对称性和零迹；r=0 保留潮汐梯度；v=0 保留 Lense–Thirring/de Sitter 的非零速度敏感度；反对称性、三个方向混合的惯性轴旋转下的 `R J Rᵀ`、非法／奇异／非有限输入，以及安装后 SDK 的实际链接调用。

```text
orbitforge_core_tests --gtest_filter=ForceDifferential.*:ThirdBody*.*:Relativity.*
```

这些用例只验收力及其局部状态偏导，不替代完整协方差传播、consider analysis、事件跳变和 STK 同配置实测。后续变分方程须继续提供独立 STM／协方差轨迹参考。
