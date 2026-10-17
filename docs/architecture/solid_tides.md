# 固体潮

HPOP 的固体潮由共享 Core 计算，桌面、Python、C++ SDK 和任务序列使用同一实现。当前支持 STK Gravity 页的 None / Permanent tide only / Full，以及 Truncate to Gravity Field Size 和 Include Time Dependent Terms。Full 默认计算日月主要形变，可另启用 71 个频率修正项和固体极潮。最小振幅筛选仍待实现；文件显式长期变化和海洋潮见各自契约，不能将本阶段视为完整 STK 等价实现。

## 计算模型

依据 [STK Gravity](https://help.agi.com/stk/Content/hpop/hpopGravity.htm)、[IERS Conventions 2003 第六章](https://www.iers.org/fileadmin/SharedDocs/Publikationen/EN/IERS/Publications/tn/TechnNote32/tn32_057.pdf) 的公式 (1)、(2)、(12) 及表 6.1。采用非弹性地球 Love 数：k20=0.30190；k21=0.29830−0.00144i；k22=0.30102−0.00130i；3 阶前三项 0.093、m=3 为 0.094；诱导 4 阶 k2m+ 为 −0.00089、−0.00080、−0.00057。k20 默认使用 [STK 重力文件文档](https://help.agi.com/stk/Content/stk/importfilesGravity.htm)所述 2006 年更新后的 IERS 值 0.30190；外部 `.grv` 可以显式覆盖其实部。其他 Love 数、频率修正表与极潮常数保持 IERS 2003 定义，不随 k20 整体缩放。

日月几何地心位置在每次积分阶段转换到同一 ITRF 系，使用所选星历解的 Sun/Moon GM，与直接第三体引力的启用和 GM 覆盖独立。归一化系数为 δ(Cnm−iSnm)=knm/(2n+1) Σ[GMj/GME (R/rj)^(n+1) Pnm(sinφj) exp(−imλj)]；2 阶形变还诱导 4 阶系数。实现使用显式笛卡尔表面谐函数，日月位于极点时没有经度除零。

永久 C20 为 4.4228e−8 × (−0.31460) × k20，默认值为 −4.20067548472e−9。自定义 k20 同时用于日月二阶零次响应和可分离永久形变，不能只改其中一个。tide_free 底场不含该项；zero_tide 底场在启用 C20 时已经包含该项。None 删除可分离的永久形变；Permanent 仅保留一次；Full 的瞬时日月响应已含永久成分，不再另加。0/0 底场没有 C20，因而不从修正中扣除不存在的底场永久项。mean_tide 当前明确拒绝，避免将直接永久潮和地球形变混为一谈。

截断开启时按静态重力的 degree 和 order 分别筛选潮汐系数；0/0 不产生潮汐力，也不要求相关星历/EOP。截断关闭时可计算至 4 阶、3 次，即使静态文件只到 2 阶、0 次，或当前选择 0/0。静态场对象保持不可变；`GravityModel::acceleration` 接收当次加性系数，既不克隆整份高阶系数表，也不改变下一次静态求值。该通用接口检查重复项、有限数、归一化索引及 2190 阶计算上限。

## 附加时间项

`include_time_dependent_terms` 仅在 Full 且有效潮汐阶次至少为 2 时生效，默认关闭；同时控制频率项和固体极潮，对应 STK 的单一开关。其余模式保存该草稿，但不要求额外资源，也不改变计算身份。每个积分阶段读取同历元 EOP，不重用初始历元的修正。

频率修正使用 IERS 2003 表 6.3a/b/c 的 48/21/2 项，仅改变 C20、C21/S21、C22/S22。表中振幅单位 1e−12；相位为 `m × (GMST + π) − Σ(表列 Delaunay 乘子 × 基本角)`。Delaunay 多项式采用 TT 世纪，GMST 使用 IERS 2003 表 5.4（IAU 2000）的多项式和 UT1 地球自转角；UT1 从 EOP 的 UT1−TAI 直接构造，先拆整数日与日内部分以保持精度，不把 UTC 当 UT1。

极潮使用 IERS 2003 第六章原文的 `δC21=−1.333e−9(m1−0.0115m2)`、`δS21=−1.333e−9(m2+0.0115m1)`，其中 `m1=xp−mean_xp`、`m2=mean_yp−yp`，单位角秒。**这与 IERS 2010 修正后的交叉项符号不同。** 当前选择 2003 原文以明确遵循 STK 帮助所指规范；尚未用 STK 数值导出确定具体版本是否采用后续修正，不能据文档名称声称逐位一致。

平均极点使用 IERS 2003 `annual.pole` 的 1900–2000 年样本。各年 1 月 1 日 UTC 由注入的时间 Provider 转成 TAI 后线性插值；2000-01-01 UTC 的末端样本保留，此后采用 `mean_xp=.054+.00083t`、`mean_yp=.357+.00395t`（角秒），t 为自 J2000 TT 起的儒略年。表与线性外推在末端存在规范本身的不连续；测试覆盖端点及两侧。早于 1900 年明确拒绝；UTC、EOP 和星历各自的实际覆盖仍须满足，不静默将缺少极潮当零。历史插值也要求时间 Provider 覆盖相邻两个年初。

数值表固定在 `solid_tide_data.inc`，来自 IERS 2003，转录依据为 [Orekit 13.1.2 原始资源包](https://repo.maven.apache.org/maven2/org/orekit/orekit/13.1.2/orekit-13.1.2-sources.jar)的 `assets/org/orekit/IERS-conventions/2003/`，包 SHA-256 `e83fc14e95e1c92fe7da978e0c99e3d90c670a5e32292bb89b3d27159ee3983c`。表 6.3a/b/c 原文资源 SHA-256 分别为 `b6a3ccdec48fa08ec1a8a704ea620a081d4be571127e5e0d8bc19b439f75a212`、`96ad401b3cea47a8ae62c942256dbdd356c7a59d350372f397b90866c4093b31`、`5bac1f2cc2cffa2720fc32bc6c0b8fbac2cff67e52703cf0b072d8b8437c38e5`；`annual.pole` 为 `9239c0b841fb55bb5ac1ddebbea9fc628f1748b1366c5ac9b841046e74155fa8`。只转录规范数值，运行时不依赖 Orekit，也不联网。

## 接口与来源

C++ 使用 `HpopOptions::solid_tides`（`SolidTideOptions`），Python 使用 `of.orbits.SolidTideSettings(mode="full", truncate_to_gravity=False, include_time_dependent_terms=True)` 并传入 `Hpop(..., solid_tides=...)`。SDK/Python 默认 None；桌面新配置默认仅永久潮，按参考截图。完整模式独立要求 Sun/Moon 星历和 EOP/Frame Provider，覆盖校验包括初始历元至全部积分路径，计算核心不联网。

模型标识 `iers2003-anelastic-v3` 记录模式、截断、k20、频率/极潮约定及其开关、实际日月 GM 及星历/EOP 身份。项目 v40 / OBJS31 保存三个选项及非活动草稿；修改设置不改变旧分析。只接受当前格式。重力元数据 `GravityInfo::solid_tide_k20` / Python `GravityInfo.solid_tide_k20` 保存实际实部；HPOP 引力页和数据中心只读显示该值，不增设与文件配置相互覆盖的编辑项。重力目录收据使用 schema 3，明确保存 k20 与可空的长期变化元数据；旧收据须重新导入或下载，不按旧格式补默认值。Love 数从资源元数据读取，不额外写入项目对象字段；原文件 SHA-256 与 `normalized-gravity-2190-secular-julian-year-v4` 解释版本进入数据引用，改变后沿用确认流程。

### STK 文件中的 Love 数

读取 `Begin EarthIERSTideModel` / `End EarthIERSTideModel` 中的 `LoveNumbers 2 0 <real> <imaginary>`，也支持 STK 文档列出的 `Begin LoveNumbers` / `2 0 <real> <imaginary>` / `End LoveNumbers` 语法。只允许 2 阶 0 次，两个数都必须有限；按 STK 定义只使用实部，有限虚部可以非零。k20 为无量纲实数，0 合法，不额外施加未公开的范围限制。未指定时使用默认值；缺失或损坏的行不能降级为默认值。

`IncludesPermTide` 可在头部或 EarthIERSTideModel 中给出；两处同时给出必须一致，各处重复声明拒绝。空 EarthIERSTideModel 使用默认 k20；空 LoveNumbers、重复/嵌套块、未知记录及不匹配的结束标记明确拒绝。`CentralBody Earth` 可显式声明，其他中心天体、SimpleTideModel、ReferenceFrame 与 ProcessNoise 块尚未支持，继续明确拒绝。新增语法只影响潮汐元数据，静态球谐系数保持不变。

## 验证

`tools/validation/orekit/OrbitForgeSolidTideReference.java` 调用未修改的 Orekit 13.1.2 `SolidTidesField`、IERS_2003 Love 数、Holmes–Featherstone 与 Hipparchus 4.0.2 DP853。固定结果为 `tests/reference/propagators/solid_tides.txt`：120 组加速度（两种底场潮汐系统、三组日月几何、五组静态截断、潮汐截断开关、普通/近极点卫星）；四组移动日月 ±6000 s / ±1 天传播，关闭直接第三体引力。加速度逐分量容限 2e−14 m/s²（浮点球谐舍入），传播 3 mm / 3 μm/s（两种积分器误差控制差异）。另验证永久潮解析 J2、极点、低阶文件的扩展、覆盖不足、错误恢复及不可变底场。

自定义 k20 的独立数据为 `solid_tides_love.txt`，由 `OrbitForgeLoveNumberReference.java` 向原版 Orekit `LoveNumbers` / `SolidTidesField` 传参生成：k20=0、0.3、0.30190、0.4；48 组系数、96 组加速度、16 条 ±1 天轨迹，覆盖 tide-free / zero-tide、独立截断、近极点和附加时间项开关。永久项由 Orekit 的 IERS 值按同一 k20 换算；其余 Love 数和频率/极潮直接来自库。沿用系数 2e−20、加速度 2e−14 m/s²、位置 3 mm、速度 3 μm/s 容限。格式、事务、数据确认、分析不可变、Python 和 SDK 另行验证。

这些是独立 IERS/Orekit 对照，尚无同配置 STK 轨迹导出实测；不能把关闭的附加时间项、不同极潮符号或不同 Love 数约定的结果视为相同模型。

附加时间项的独立参考为 `solid_tides_time_dependent.txt`，由 `OrbitForgeTimeDependentTideReference.java` 调用未修改的 Orekit `IERS_2003` 频率/极潮函数、`SolidTidesField`、Holmes–Featherstone 和 DP853 生成。使用明确的合成 UTC=TAI−32 s、恒定 EOP 和单位旋转，以隔离力模型；39 组系数覆盖 1972–2026、纳秒余数、历史平均极点及 2000 边界，24 组加速度覆盖 0/0、2/0、2/1、2/2、4/3、21/21、截断开关及近极点，4 条 ±6000 s/±1 天轨迹。系数绝对容限 2e−20，依据大历元基本角的浮点舍入；加速度及轨迹沿用上述容限，没有放宽。Python 另使用真实 DE440s/EOP/闰秒表核验启用开关后的身份、报告、保存与重开。
