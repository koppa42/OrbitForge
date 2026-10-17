# HPOP 海潮与海洋极潮

海潮位于力模型的 Gravity 页，采用 STK 的独立 Degree / Order 语义。默认关闭，草稿为 FES2014bv1、4×4；启用后真实改变加速度与轨迹。固体潮仍采用其独立的 IERS 2003 约定，海潮采用 IERS 2010；两者合并加性球谐系数后一次计算场加速度，避免重复索引或被中心引力的截断误删。

## 模型与范围

- 官方文件来自 [GRGS / FES2014b.v1 with S1](https://grace.obs-mip.fr/dealiasing_and_tides/ocean-tides/)，最高 100×100，36 个显式 Doodson 分潮，180272 条记录。v1 的 Sa、S1、MSf/MSq 修正已在源文件中，不另加相位改正。完整原文 SHA-256 为 `74156304aca8f66ed1f67775de3d4a9d24ecb94abb47127ef40d5bb4630f719f`。
- 该文件给出经负荷 Love 数转换的完全归一化势系数，不是海面高度。采用 GM=3.986004415e14 m³/s²、R=6378136.3 m。输入单位标记为 10^-11；外部 POT 也可声明 10^-12，不凭数值大小猜单位。
- 外部文件和核心支持至 2190 阶；实际阶次须在资源范围内，超限报错。各分潮可以有不同的完整矩形范围，例如 Om2 仅 2/0；同一分潮范围内缺项、重复、非有限数、未识别的文件头或水高格式明确拒绝。
- 不推算缺失的次要分潮，不提供尚未实现的 Min. Amplitude 筛选。[AGI 的 FES2014bv1 说明](https://help.agi.com/odtk/content/od/ODObjectsSatelliteGravityModelTides.htm)还涉及扩展次要分潮；不能将当前 36 个显式分潮声明为与该扩展模型完全一致。

## 数值约定

计算采用 [IERS 2010 第 6 章](https://iers-conventions.obspm.fr/content/chapter6/icc6.pdf)式 6.15。每个分潮具有 Doodson 频率编码与 C+、S+、C−、S−；相位由 UT1 的 GMST 与 TT 的 Delaunay 基本参数构成。令分潮相位为 θ，对每对 n,m 累加：

```text
ΔCnm = (C+ + C−) cos θ + (S+ + S−) sin θ
ΔSnm = (S+ − S−) cos θ + (C− − C+) sin θ
```

m=0 的 S 规范为零，其势函数项恒为零。TAI 纳秒历元精确转换到 TT（+32.184 s），UT1 由 EOP 的 UT1−TAI 得到；地球转角拆分整日和日内秒，避免直接将整个历元舍入为一天的小数。

按 [STK Gravity 帮助](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)，海潮阶数至少 2、次数至少 1 时自动加入海洋极潮。采用 IERS 2010 式 6.24、原版三次/线性平均极点模型，分界为 2010-01-01 00:00 TT，含分界本身的三次段。m1=xp−mean(xp)，m2=mean(yp)−yp，单位为角秒：

```text
ΔC21 = −2.1778e−10 (m1 − 0.01724 m2)
ΔS21 = −1.7232e−10 (m2 − 0.03365 m1)
```

这与固体极潮是不同物理项。原版平均极点约定写入算法/数据解释身份，不静默替换为后续 IERS 修订。核心低层 `corrections(..., include_pole=false)` 仅供分项验证，HPOP 无关闭自动海洋极潮的独立开关。

海潮系数按 `(GM_ocean / GM_base) × (R_ocean / R_base)^n` 转换到所选底场，再与固体潮系数相加。海潮不受底场阶次限制：底场可为 0/0，海潮仍取 4×4；固体潮的截断开关也不截断海潮。所有系数在 ITRF 求场，经明确的 Frame/EOP Provider 转回 GCRF；分项引力参数与卫星质量无关。

## 接口、所有权与持久化

`OceanTideOptions` 保存 enabled、model、degree、order。ID 为非空的小写 ASCII 字母/数字/连字符，最长 64；degree 0–2190，order 0–degree。禁用和 0/1 阶保留合法草稿，不依赖文件、无海潮力，也不改变计算身份。活动模型需要覆盖积分路径的 EOP 和帧 Provider；单独海潮不要求 Sun/Moon 星历或 UTC Provider。

`OceanTideModel` 构造时拥有并校验全部系数，此后不可变；并发 `corrections` 使用局部累加存储。输入或缩放无效抛出 `invalid_argument`，不静默降低阶次。`runtime::load_ocean_tides(path)` 完整读取原文、校验并返回共享只读模型；最多 128 MiB、4096 个分潮、400 万条记录。加载与目录安装支持取消检查点。应用层使用 [数据中心](data_center.md#海潮模型)管理下载和导入，传播核心不执行 I/O。

Python 易用入口：

```python
import orbitforge as of

data = of.DataSnapshot.load(eop_manifest="eop/manifest.json").with_ocean_tides(
    "fes2014bv1", "fes2014b.v1.with-s1.potential.txt")
settings = of.orbits.OceanTideSettings(enabled=True, model="fes2014bv1", degree=4, order=4)
# of.orbits.Hpop(initial=..., ocean_tides=settings)
```

`OceanTideSettings` 是不可变值；低层 `OceanTideOptions` 对应 C++ 参数，`OceanTideModel.info` 是只读元数据。目录可 `install_ocean_tides(path, selection)`；官方 ID 只接受固定完整原文，修改后的外部文件必须使用自己的 ID。

项目 v40 / OBJS31 保存全部草稿，仅支持当前格式。活动参数进入输入身份，`ocean-tides/<ID>` 保存完整原文 SHA-256 与 `fes-potential-iers2010-original-mean-pole-v1` 解释身份，缩放常数进入配置哈希。重新准备时数据变化须确认；已经创建的分析、报告和轨迹持有原模型。设置取消不发布草稿，下载失败不替换已有数据。

## 数值验证与未覆盖范围

独立参考调用未经修改的 Orekit 13.1.2 `FESCnmSnmReader`、`OceanTidesField`、`HolmesFeatherstoneAttractionModel` 和 Hipparchus 4.0.2 DP853，不调用 OrbitForge 计算代码。固定小文件覆盖全部 36 个显式分潮的 4×4 截取；可选完整文件覆盖 100×100。另将 Orekit 的固体潮和海潮作为两个力分别积分，检查 OrbitForge 系数合并及独立截断。

系数每分量容限 2e-20，加速度 2e-14 m/s²；轨迹位置 1 mm、速度 1e-6 m/s。前两项覆盖双精度相位/求和舍入，后两项覆盖独立积分器在相同严格容差下的全局误差；不代表真实预报误差。完整输入、边界、生成命令和接口测试见[验证协议](../validation/hpop_protocol.md#海潮与组合潮汐)。

参考使用固定同轴地球隔离潮汐算法。真实 EOP 的数据装配、保存、报告和确认另有集成测试；完整旋转地球的 STK 同配置轨迹、次要分潮推算、最小振幅筛选尚未完成。该功能也不包含站点海潮负荷位移或 EOP 日内海潮改正。
