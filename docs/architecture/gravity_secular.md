# 引力系数长期变化

HPOP 引力页提供“包含引力系数长期变化”开关，位置和职责沿用 STK 的中心天体引力设置。当前实现文件显式指定的线性模型；桌面、C++、Python、分析与报告使用同一份冻结资源。

## 模型与当前范围

[STK HPOP Gravity](https://help.agi.com/stk/Content/hpop/hpopGravity.htm) 将 EGM96/EGM2008 的 C20、C21、S21 演化与 IERS 2003 关联；[STK Components 重力文件读取说明](https://help.agi.com/AGIComponentsJava/Javadoc/agi-foundation-celestial-SphericalHarmonicGravityModel.html) 公开了 RefEpochTDT / LinearRate；[线性模型接口](https://help.agi.com/STKComponentsJava/Javadoc/agi-foundation-celestial-SphericalHarmonicGravitySecularVariations.html) 明确参考历元处变化为零、变化率按年给出。Components 的公开接口不是 STK Desktop 同配置轨迹真值。

对文件定义的每个归一化系数：

```text
C_nm(t) = C_nm(reference) + C_rate_nm × (t - reference) / 31557600 s
S_nm(t) = S_nm(reference) + S_rate_nm × (t - reference) / 31557600 s
```

本实现的一年是 365.25 SI 天。RefEpochTDT 是 TT 儒略日；导入时先分离整数日与小数日，再按 TT = TAI + 32.184 s 转成 Epoch 纳秒并舍入，避免先把整个 JD 转为 double 带来的数十微秒损失。传播中使用连续 TAI 时差；没有 UTC 年长、闰秒或卫星初始历元参与率的换算。

基准系数必须对应文件的参考历元。开关关闭时使用原基准场；开启时在每个积分阶段历元计算附加修正，绝不修改共享基准数组。率按中心场 degree/order 截断，固体潮与海潮沿用各自截断；三种修正在相同 n,m 上相加。

当前官方 ICGEM EGM96/EGM2008 下载和内置示例没有显式率/历元，不添加猜测参数。IERS 2003 的基准 C20/C21/S21 与原始 EGM 静态系数不同，不能直接移植一组变化率并任意选择起点。静态 EGM 产品的 STK 默认演化约定仍是对齐清单中的未完成项；本能力不代表其已完成。WGS84 球谐模型继续暂缓。

## 文件与 API

`.grv` 可在 Coefficients 块之前或之后包含一次：

```text
Begin SecularVariations
RefEpochTDT 2451545.0
LinearRate Cos 2 0 1.162755e-11
LinearRate Cos 2 1 -0.337e-11
LinearRate Sin 2 1 1.606e-11
End SecularVariations
```

此片段只示范文件语法，不是向任意 EGM 文件直接追加的默认配置。文件的 Normalized 设置同时作用于系数与率，No 时转换为完全归一化；无 Condon–Shortley 相位。未声明的率为零。支持 2–2190 阶且不超过文件实际阶次，Cos/Sin 各分量最多一次。必须有一个参考历元及至少一条率记录。未知记录、重复块/分量、未闭合块、非有限值、非零 S_n0、越界、归一化溢出和超出 Epoch 范围均拒绝。块不能嵌套或插入系数记录中。ICGEM gfct/trnd 等时变格式仍未接入。

C++ 的 `GravityInfo::secular_variations` 保存可空的 `GravitySecularVariations`（Epoch 与 `rates_per_year`）；构造模型时校验、复制并排序。`GravityModel::secular_corrections` 返回指定历元及截断的附加系数，`acceleration` 的可选 `secular_at` 参数使其与外部潮汐修正共同求值。缺少元数据、非法截断或演化后溢出明确报错。

HPOP 与 Python `orbits.HpopSettings` / `orbits.Hpop(...)` 使用布尔 `include_secular_variations`，默认 false。Python 目录元数据只读，公开参考历元及逐项变化率。非中心重力启用该项而资源缺失率/历元时准备失败。0/0 下保留草稿，不要求长期变化数据，不改变确定性轨迹与输入身份。

## 数据、交互与保存

变化率与参考历元属于重力资源，项目只保存开关。数据目录收据 schema 3 保存完整元数据，旧收据明确拒绝并提示重新导入/下载；不补默认值。引用版本为 `normalized-gravity-2190-secular-julian-year-v4`，原文字节及物理解释进入身份。更新、重新准备或重开时沿用数据变化确认；取消/失败保留原资源，运行中分析保持原系数与历元。

引力页只读显示参考 TT JD 和率数量。已知文件缺少元数据时不允许新启用；从可用文件切换到静态场后保留已勾选草稿，让用户关闭或更换文件，确定时校验并定位。切换页、宽窄布局和单位不会重建控件；取消不提交。数据中心同时显示上述元数据。

项目 v40 / OBJS31 在重力 degree/order 后保存一个严格 0/1 字节。只接受当前格式；独立二进制样例保留非活动 true 草稿，跨 C++/Python 精确往返。有效开关进入分析、报告和会话算法身份。

## 独立验证

`OrbitForgeSecularGravityReference.java` 直接调用未修改的 Orekit 13.1.2 `SecularTrendSphericalHarmonics`、Holmes–Featherstone、NewtonianAttraction 与 Hipparchus 4.0.2 DP853；不用 OrbitForge 解析器、力模型或积分器生成真值。三份固定的合成 .grv 包含不同 TT 参考历元及归一化方式，具有完整字节散列，且不是 STK 默认数据。

参考包含 48 组系数、288 组加速度与 48 条正反向一天轨迹：跨参考历元两侧、百年时差、2/0、2/1、8/4、一般位置、近极区与高轨、开关两态。每分量容限依次为 2e−23、2e−13 m/s²、1 mm、1e−6 m/s，覆盖双精度归一化/求和与两个独立积分器的差异，不代表物理预报精度。

`OrbitForgeSecularTideReference.java` 将长期变化的底场、完整固体潮及 4×4 海潮分别作为 Orekit 独立力积分；8 条正反向一天轨迹检验相同系数的合并与独立截断，位置/速度沿用 1 mm / 1e−6 m/s 容限。设置、生成和复现步骤见[HPOP 验证协议](../validation/hpop_protocol.md)。
