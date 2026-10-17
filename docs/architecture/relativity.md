# HPOP 相对论加速度

STK 13.1 的 [Additional 帮助](https://help.agi.com/stk/Content/hpop/hpopAdditional.htm) 将 Include Relativistic Accelerations 定义为 IERS Technical Note 32（2003）的广义相对论修正。OrbitForge 的 `HpopOptions.include_relativistic_accelerations` 默认关闭；开启时同时加入 Schwarzschild、Lense–Thirring 和 de Sitter 三项，C++、桌面、Python、任务序列及报告共用 Core 实现。这里采用公开方程，不声称复现 STK 内部实现或完成同配置 STK 轨迹验收。

## 方程与输入约定

依据 [IERS Conventions 2003 归档](https://iers-conventions.obspm.fr/archive/2003/2003IERSConventionsArchive.tar.gz) 中 `chapter10/tn32_c10.pdf`，第 10.2 节式 (1)，β=γ=1。归档 SHA-256 为 `1c7ac5c46f49dbfc6e140a34228067b480796ac11e3348ab41905e5af119f708`，章节 PDF 为 `2f9af15a7c9d50d0c68786325707140ea475bf59e3b93e447f2d70592e58a385`。

令 **r、v** 为卫星地心惯性位置与速度，**s、u** 为太阳相对地球的几何位置与速度，**k** 为地固 +Z 轴在相同惯性轴中的单位方向。全部输入使用 SI，速度是对 TAI 秒的导数；TT 与 TAI 仅相差常数，导数相同。令 **J** = 9.8×10⁸ **k** m²/s，c = 299792458 m/s，μ 和 μs 分别为选定地球重力场及星历解的太阳 GM。

```text
Schwarzschild = μ/(c² |r|³) [(4μ/|r| − v·v) r + 4(r·v) v]
Lense–Thirring = 2μ/(c² |r|³) [3(r·J)/|r|² (r×v) + v×J]
de Sitter      = 3μs/(c² |s|³) [(s×u)×v]
```

IERS 原文 de Sitter 使用地球相对太阳的 R、V；这里两向量同时反号，叉积不变，已独立对照符号。J 是角动量除以地球质量，并非惯性矩或角速度。`gcrf_to_itrf(...).inverse_position({0,0,1})` 在每次力评估中给出自转轴方向，使用所选 EOP 与 Frame Provider。没有添加相对论多极项，也不实现完整 BCRS/GCRS 坐标或质量尺度转换。

太阳输入为 DE440s 的 Earth-relative、ICRF-aligned 几何状态，不使用光行时或光行差。CSPICE 原始速度标识为每 TDB 秒；新接口 `IEphemerisProvider::tdb_seconds_per_tai_second(Epoch)` 根据同一不可变 LSK 的解析时间导数给出 dTDB/dTAI，Runtime 显式乘到速度上，再通过 `IThirdBodyEphemeris::geometric_state` 返回 `GeometricBodyState`。原始 `EphemerisState` 的 SPICE J2000/TDB 标签保持不变。这个牛顿星历输入近似不能表述为完整的相对论状态变换。

## 依赖、接口与文件

开启后即使静态重力为 0/0、第三体引力全部关闭，也必须注入覆盖初始至查询完整路径的 EOP、Frame 和太阳星历。太阳 GM 来自星历解，不受未启用的第三体 GM 草稿覆盖。缺失速度能力明确报错；不偷偷差分位置、不补零速度，也不在计算中下载数据。范围、内部星历缺口及无效输入均由共享校验拒绝。`evaluate_relativity` 拒绝非有限输入、非正 GM、零距离、非单位自转轴及达到光速的速度；返回每项与总加速度，便于诊断和数值对照。

Python 使用 `of.orbits.Hpop(..., include_relativistic_accelerations=True)`，或不可变 `HpopSettings` 的同名严格布尔字段；`of.evaluate_relativity` 调用同一个核心函数。桌面在“力模型 → 其他 → 相对论”提供开关与模型说明，取消不提交草稿。

项目 v40 / OBJS31 在海潮设置之后保存一个严格布尔字节；旧版本明确拒绝。物理输入哈希包含开关，启用后的来源标识包含 `iers2003-three-terms-v1`、三项常数、ITRF 极轴约定、速度时间尺度、地球/太阳 GM 和所选 EOP/星历身份。DE440 解释版本为 `de440-newtonian-geocentric-icrf-none-gm440-planets-tai-rate-v3`。开关或输入变化不会改变已经捕获的分析快照。

## 独立数值验收

`tools/validation/orekit/OrbitForgeRelativityReference.java` 调用未经修改的 Orekit 13.1.2 `Relativity`、`LenseThirringRelativity`、`DeSitterRelativity` 和 Hipparchus DP853，既不导入 OrbitForge，也不调用生产公式。Orekit 对应的 IERS 2010 相对论三项与上述 IERS 2003 式 (1) 相同。JAR 固定版本、来源和 SHA-256 见同目录 `dependencies.json`。编译：`javac -cp <jars> -d <output> tools/validation/orekit/OrbitForgeRelativityReference.java`；运行：`java -cp <jars+output> OrbitForgeRelativityReference`。classpath 分隔符按平台选择，标准输出仅将换行统一为 LF。

固定输入 `tests/reference/propagators/relativity.txt` 的 SHA-256 为 `b3d6c74b77b0dedb0ee25881ac0b7e291f41b44667bdcf0aed1d7905d05e2339`：

- 45 组分项加速度：三组太阳历元、三个极轴倾角、低轨/近极点/倾斜/GEO/零速度状态；Schwarzschild 与总量分量容限 2e−22 m/s²，另两项 2e−24 m/s²。这些界限覆盖双精度舍入，不通过相加抵消掩盖单项错误。
- 8 条传播轨迹：三项全开或全关、±6000 秒与 ±86400 秒，地球点质量、固定倾斜极轴、运动几何太阳、直接第三体引力关闭；统一 TAI 秒。初态 r=(7000000,0,0) m，v=(0,6200,5300) m/s。独立 DP853 的位置/速度绝对容差为 1e−6 m / 1e−9 m/s，相对 1e−14，最大步长 30 秒；OrbitForge RKF78 为 1e−5 m / 1e−8 m/s、相对 1e−13、最大 30 秒。验收位置分量 0.1 mm、速度分量 1e−7 m/s，允许两种独立积分器的截断和舍入差异。
- `Spice.MovingSunLightTimeAndAberration` 使用放大变化率的人工 LSK 检验导数方向和数值；`EphemerisFiles.GeometricVelocityUsesTaiDerivativeAndRelativityRecordsItsInputs` 检验真实 DE440s 的速度转换、位置导数与实际传播来源。这些路由测试与合成力模型真值分开。
- Python 安装后验证 45 组数据、真实 DE440s/EOP 传播、报告一致性、开关身份、存档精确重建及旧分析不可变；固定项目样例独立组装，并对重算 CRC 的非法布尔值拒绝。SDK 消费端调用独立分项接口；Qt 检查键盘、宽窄/DPI、重开/取消、缺数据错误定位。

这组参考隔离了星历和参考系模型差异；实际旋转地球的 STK 同输入导出对照仍待补充，不能将微米级合成轨迹差异宣称为真实轨道预报精度。
