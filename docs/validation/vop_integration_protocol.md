# VOP 积分数值对照

实现契约见 [VOP 积分](../architecture/vop_integration.md)。数值测试以相同 SI、GCRF/固定轴、TAI、初态和力模型比较，不用另一条生产路径生成参考答案。

## 独立二体流及状态转移矩阵

`tools/validation/orekit/OrbitForgeKeplerJacobianReference.java` 使用锁定的 Orekit 13.1.2 / Hipparchus 4.0.2。Cartesian NumericalPropagator 在 DP853 中独立积分纯 Newtonian 运动及变分方程，由 MatricesHarvester 读取 STM；没有调用 OrbitForge。依赖散列见同目录 `dependencies.json`。

固定输入 `tests/reference/propagators/universal_kepler_stm.txt` 含 88 行：已有 80 个圆/椭圆/近抛物/抛物/双曲线状态与双向时间，加整圈、多圈、零时间和半周期两侧。ASCII/LF，SHA-256 `4427bfdf3260aa4826e09b4153d29a7ea99d4f418ac5582c774b65a6d14523db`。参考生成同时将最大步长 10 s 减半为 5 s；最大差为 1.162e-5 m、1.186e-8 m/s、5.801e-10 无量纲 STM。

`UniversalKepler.DifferentialMatchesIndependentOrekitAndSymplecticInverse` 以初始半径 R 和速度 sqrt(mu/R) 缩放矩阵。独立 STM、逆流复合、辛条件误差均须小于 2e-8；位置/速度参考容限 5e-4 m / 5e-7 m/s。另检查单独保存的映射增量、普通二体映射结果一致性及取消。

可复现命令（classpath 分隔符依平台调整）：

```text
javac -cp "jars/*" -d classes tools/validation/orekit/OrbitForgeKeplerJacobianReference.java
java -cp "classes;jars/*" OrbitForgeKeplerJacobianReference tests/reference/propagators/universal_kepler.txt universal_kepler_stm.txt
```

## 完整积分及摄动

- `HpopVop.OneLargeStepMatchesOrekitAcrossAllConics`：两种积分器分别覆盖 80 个 Orekit DP853 固定值；固定步长 86400 s，一次候选步到输出终点，位置范数 5e-4 m / 速度分量 5e-7 m/s。这验证二体映射实际参与积分。
- `HpopVop.MatchesIndependentHipparchusWithPerturbationsAndRegularization`：60 个 Hipparchus GBS 圆轨道/偏心轨道、J2、随时间运动月球固定值 × 两种方法 × 固定/自适应 × 普通/正则化，共 480 组。双向至一天；位置范数 0.005 m、速度范数 5e-6 m/s。参考生成与来源沿用 `bulirsch_stoer.txt` 的既有协议。
- NRLMSISE-00 阻力轨迹与 Orekit 固定值使用原有 0.0005 m / 5e-7 m/s 分量容限；RKF78、BS 均覆盖 Cowell/VOP。
- 光压的独立 Orekit 阴影事件及终点轨迹覆盖 LEO/GEO、圆柱/双锥、正反向、两种步长模式、正则化，使用既有事件 3e-7 s、位置 0.001 m、速度 1e-6 m/s 上限。重复减半步长，误差需收敛或维持已规定的参考/舍入误差平台，不放宽既有数值门槛。
- `SundmanBounded.AdaptiveMinimumStepRejectsUnresolvedOscillatoryForce` 使用已有高频摄动力，验证 VOP 的物理误差尺度、Min Step 拒绝、尝试次数上限和原状态保留；不以某个大步长必然失败为前提。
- Hohmann 双脉冲验证每段 VOP 重新起步，解析转移半径/速度和累计 Delta-v；闰秒天气跳变验证不跨边界取样，保留独立细步 Cowell 对照。

## 公共接口及交付

C++ / 安装后 Python 验证有效/非活动开关、独立插值 μ、分析/报告/Session 身份、旧分析不可变、精确纳秒输出、取消重试及重开。独立 struct.pack 项目样例保存非活动 true 草稿并逐字节重写；修复 CRC 的非法布尔值及全部非当前版本拒绝。安装 SDK 编译消费者调用 VOP；Qt 检查只在 RKF78/BS 启用、键盘操作、切换保留、取消不提交，以及 430/720 逻辑宽度和实际 DPI。运行日志、误差指标、截图和安装环境放在独立 `out/validation/` 目录。


当前参考组合的误差记录：无量纲 STM 最大差 5.1751e-10、逆流复合误差 1.9094e-11、辛条件误差 2.4784e-11；480 组轨迹最大位置差 1.1565e-3 m、速度差 1.2207e-6 m/s。VOP 光压阴影对照最大位置分量差 1.7027e-5 m、速度差 6.835e-9 m/s，事件时间差 1.448e-9 s；VOP 阻力对照最大位置分量差 9.4252e-5 m、速度差 1.2444e-7 m/s。以上是固定测试组合的数值差，不能外推为任意实际轨道的预报精度。
