# 统一数据中心

当前实现包括 [Scenario 数据锁](scenario_data_lock.md)、SQLite 统一目录、左列表、右详情的数据管理窗口及有限 EOP 外推、IERS finals2000A 全量下载和应用运行期间的自动更新。C04 和 UTC／闰秒同样支持全量下载、独立自动更新及本地原文导入；地球重力场支持 EGM2008 / EGM96 全量下载和静态外部文件导入，太阳系星历支持 DE440s 全量下载、显式快照与行星本体 SPK 导入；海潮支持 FES2014bv1 POT 下载和外部 POT 导入。桌面首次启动必须准备完整基础数据；GP/OMM/TLE 的数据中心管理尚未接入。

## 管理边界

应用级“数据中心”按数据类型管理当前文件、获取时间、覆盖范围和更新操作。左侧采用两级目录：“地球定向参数”下是 finals2000A 与 C04；“UTC / 闰秒”是独立条目；“地球重力场”下是 EGM2008、EGM96 与外部重力场。海潮模型独立分组，含 FES2014bv1 与外部海潮文件。底层 `DataInventoryItem` 保留各自的安装状态、获取记录、覆盖范围、文件信息和操作能力；分类与来源身份独立于 Provider 下标。普通用户不管理历史版本或默认版本，清单由适配器生成。

安装状态区分未下载、内置示例、本地文件、完整包已下载、程序内置。安装、时间覆盖和联网更新结果彼此独立：内置 EOP 示例不算完整包，能覆盖今天不代表联网确认过最新版，历史 EOP 的覆盖结束也不表示文件失效。UTC 的有效截止保留明确的排他边界。

新建场景使用当前数据。打开或重新计算时先检查上次数据身份：相同则继续，不同则说明变化并请求用户确认；允许后使用当前数据重建，取消则不打开或重算，原场景及结果保持不变。默认计算顺序是 C04 → finals2000A，可选中“地球定向参数”父级，在右侧拖动调整。历史时段优先综合参考序列；近期和预测时段由覆盖更及时的 finals2000A 提供。先按顺序寻找实际覆盖完整时段的来源，全部不足时才按相同顺序尝试允许的有限外推；优先来源缺失时使用另一来源。两份文件不拼接成一个覆盖区间。

优先级是应用级设置，以 `eop-preferred-product` 存入 SQLite settings，重启、下载与导入均保留；缺失或无效值回退到 C04 优先。修改顺序不替换活动计算中的 Provider，下一次打开或重算时重新选择；仅在实际使用的数据或外推策略变化时请求确认。优先级本身不写入 Scenario 数据哈希，项目仍记录本次计算实际使用的来源，且不要求本机保留旧文件。

一次计算及其显示缓存共用捕获的不可变 Provider；确认弹窗期间完成的下载也不会替换已展示给用户的数据。下载完成不改变已显示结果。用户允许更换数据后，一起重建全部显示轨迹并整体发布，失败或取消保留原场景。普通场景没有历史版本、回滚或手动应用数据的入口。

## 默认优先级依据

默认 C04 → finals2000A 是本项目根据产品用途作出的选择，不表示 C04 在任一日期、任一参数上都必然误差更小。

- [Bizouard 等（2019），The IERS EOP 14C04 solution](https://doi.org/10.1007/s00190-018-1186-3)：C04 是多种空间大地测量技术综合得到的 IERS 参考序列。
- [Kehm 等（2023），Combination strategy for consistent final, rapid and predicted Earth rotation parameters](https://doi.org/10.1007/s00190-022-01695-w)：区分长期最终解与低延迟快速／预测解，并讨论两者一致性问题。因此本项目按整个计算时段选择一个来源，不在中途直接拼接两个产品。
- [Kiani Shahvandi 等（2023）](https://doi.org/10.1029/2023JB026720)：快速解用于填补最终解的发布延迟，远期预测精度随时间跨度下降。
- [USNO 字段定义](https://maia.usno.navy.mil/ser7/readme.finals2000A)：finals2000A 是包含 A/B 栏的文件格式名称；当前适配器读取 Bulletin A 的观测／预测栏，并记录预测标志。[USNO 产品说明](https://maia.usno.navy.mil/ser7/readme)列出 `.all` 每周更新；每天检查不代表源文件每天发布新内容。

## 存储与引用

桌面使用 Qt `AppLocalDataLocation/data`，Windows 默认 `%LOCALAPPDATA%/OrbitForge/data`，可通过 `ORBITFORGE_DATA_HOME` 指定其他目录。指定目录后不自动搬迁旧目录；导入时仍保留原数据身份。C++/Python 可显式选择目录：

```text
data/
  catalog.sqlite       文件索引、依赖、获取记录、当前文件指针和更新状态
  blobs/sha256/...      原始文件，按内容哈希寻址、去重
  snapshots/<lock>/    manifest.json、record.json 及数据文件视图
  time/<sha256>.dat    不可变 UTC 原文
  gravity/<sha256>.dat 不可变完整重力原文
  ocean-tides/<sha256>.dat 不可变海潮势系数原文
  staging/...          尚未校验发布的临时下载
```

SQLite schema 2 在 `snapshots`、`dependencies`、`settings` 之外增加 `acquisitions`，保存获取方式与 Unix 秒时间。schema 1 升级不编造获取日期，旧数据显示“未记录”。内置数据标记“随程序附带”，没有用户下载时间。只有成功下载或手动导入才记录新的获取时间；304、取消和失败不修改它。获取记录独立于内容哈希与 Scenario 数据身份。

快照目录按依赖清单总哈希命名；record.json 保存清单原始字节的 SHA-256 与依赖身份。数据视图优先使用指向内容文件的硬链接，不支持硬链接时使用经校验的副本。finals2000A 和 C04 保存整个原文件，不按场景截取；快照内容哈希和来源哈希均为完整原文件的 SHA-256。数据库行号和本机绝对路径不写入 Scenario。每种 EOP 产品保存一个内部当前文件指针，因此同一秒内重复导入、重新导入曾安装的文件或本机时钟改变不会选错当前文件。

Runtime 的 `DataCatalog` 不依赖 Qt，提供目录查询、EOP 安装、finals2000A/C04 原文件导入、UTC 原文安装、偏好保存和按 `DataLock` 精确解析。每次加载核验清单、内容文件及时间依赖身份；EOP record schema 2 额外保存配套的 `leap-seconds.dat` 和内容哈希，因此重开目录时仍可验证对应 UTC 依赖。没有外部 UTC 文件的旧记录继续使用内置表。SQLite 使用 WAL、写事务和忙等待，串行化多个进程的发布；先验证临时副本、发布文件，再提交索引。启动时可恢复已发布而尚未写入数据库的有效快照，未发布临时目录不进入列表。Windows 发布目录时遇到临时访问拒绝或共享/锁冲突，最多额外重试 5 次，累计等待 620 ms；仍使用原子重命名，不退回部分复制。其他错误立即失败。这里不承诺断电时所有文件与数据库跨介质原子持久化。

多个场景共用相同文件，不各自复制数据。启动时按原身份导入附带快照及旧 `%LOCALAPPDATA%/OrbitForge/eop` 目录；迁移不改写已有项目文件。重复导入复用内容，已有损坏文件不会被新导入静默覆盖。损坏用户条目在窗口提示并排除加载；内置数据仍可从应用资源读取。旧内容目前作为内部缓存保留，没有用户历史管理功能；缓存清理尚未实现，项目正常打开不再依赖旧缓存。

## 全量下载与自动更新

入口为“数据 → 数据中心”，无需打开场景即可使用。左侧 EOP 默认展开，两个来源子项分别显示安装状态和覆盖截止，各来源下载中或检查异常时分别显示对应状态；UTC 条目保留有效截止。默认选中 EOP 父级，右侧显示有顺序编号和拖动手柄的来源列表，拖动后立即保存计算优先级；上移 / 下移按钮及 Alt+↑ / Alt+↓ 提供同样的排序操作。此页面说明实际完整覆盖优先于外推。

选中来源子项才显示该来源的详情与操作。左侧节点顺序固定，选中或展开节点不改变计算优先级；后台下载完成不改变所选节点，导入完成后展开 EOP 并定位到实际导入的来源。父级不展示某一份文件的安装徽标、下载按钮或覆盖日期，避免将来源状态误当作整个 EOP 类别的状态。

右侧显示所选来源的获取时间、覆盖起点、覆盖截止和最近检查结果。获取与检查时间使用本机时区，覆盖范围使用 UTC。历史覆盖结束仍可用于对应的历史场景。各来源的覆盖日期分别展示，避免把不连续或不同解算产品的时段误报为统一覆盖。

来源详情分为“概览”“更新与计算”“文件信息”三页。概览只放获取时间、计算覆盖／有效截止、最近检查和下载／导入操作；自动检查频率与有限外推放在设置页，分“自动更新”“覆盖范围之外”两组，各以开关启用，来源、路径、大小、哈希及清单导入放在文件页。切换来源回到概览。三种数据均可联网下载、更新及原文导入，各自保存检查状态和偏好；UTC 不提供 EOP 优先级或外推。没有历史版本列表或应用到场景按钮。

EOP 与 UTC 的自动下载与更新默认开启，也负责首次完整文件获取；已有用户关闭的偏好仍被尊重。启动后延迟 1.5 秒检查一次，此后仅在应用运行期间每分钟检查是否到期；每次调度依次考虑 UTC、finals2000A、C04 及已启用自动更新的重力和星历产品，串行执行到期下载，不增加常驻服务。默认距上次尝试 24 小时，可在界面选择 6 小时、24 小时或 7 天，底层支持 1–168 小时。用户可关闭自动更新或随时手动下载。关闭窗口后下载继续，退出应用会中止；失败后的自动重试遵循相同间隔。

`DataUpdateStatus` 单独记录尚未检查、检查中、下载中、已更新、无新内容、失败、取消、中断，以及更换本地文件后待检查。`finals-update-record`、`c04-update-record`、`utc-update-record` 分别以带 schema 的结构化记录存入 SQLite settings，包含尝试时间、最后成功检查时间、被检查文件的 SHA-256 和结果。重启遇到未完成状态显示“上次下载未完成”；旧目录只有自由文本状态时不推断曾成功检查。失败、取消和 304 不改下载时间，失败/取消也不改最后成功检查时间。进度条使用服务器提供的总大小；总大小未知时显示活动进度与实际接收字节数。

成功安装后保存该来源的 ETag 和 Last-Modified。再次更新使用条件请求：收到 304 时不传输文件正文、不新建快照；收到 200 时校验完整文件后发布。200 返回相同字节时记录“无新内容”和实际下载时间。条件头只用于当前文件与上次验证文件的哈希一致、来源相同的情况，不因旧缓存中存在该文件而套用到另一份导入文件。导入不同内容后显示待检查；304 返回时再次检查当前文件未变。失败或仅收到响应头时不推进验证器。来源不支持条件请求时，完整获取后按 SHA-256 去重。这里没有承诺字节级增量传输，也不把不完整的 daily 文件拼接为历史版本；网络优化不改变完整文件身份及计算来源记录。

EOP 与 UTC 下载先进入 staging，使用 HTTPS、系统证书验证、不降级到 HTTP 的重定向、30 秒无活动超时、120 秒总时限和 16 MiB 大小上限。完成后校验传输结果、文件格式、完整逐日覆盖、数据数值及时间依赖，再发布快照。成功、失败或取消时清理本次下载临时文件；进程被强制终止时可能留下不参与加载的 staging 文件。不会把半成品标记为已安装，也不修改已有场景。Windows 便携包携带 Qt TLS 后端。

下载源为 [IERS finals2000A](https://datacenter.iers.org/data/9/finals2000A.all)、[C04 operational 全量文件](https://datacenter.iers.org/products/eop/long-term/c04_operational/eopc04.1962-now) 和 [IERS Leap_Second.dat](https://hpiers.obspm.fr/iers/bul/bulc/Leap_Second.dat)。C04 全文保留，当前状态插值需要实测极移变化率，因此可计算起点不早于 1984-01-01；并验证所选 0h UTC / dX,dY 格式的全部记录。导入后的 ID 由全文件内容哈希构成，重复下载同一内容不会产生重复版本。原文件中的历史记录和不完整预测尾部全部保留；可计算范围取连续完整的 x/y、UT1、dX/dY 记录与时间 Provider 有效期的交集。导入时不对缺字段尾部补零或伪造覆盖；中间缺日、重复日、错误日期、部分缺损字段和非有限数拒绝安装。Bulletin A 的历史支持从 1972 年后的有效 UTC 起，不沿用 C04 的 1984 年限制。列定义以 [USNO 说明](https://maia.usno.navy.mil/ser7/readme.finals2000A)为准。

因此“完整文件已下载”与“任意未来日期可计算”是不同条件。界面显示实际可计算范围；UTC 超出本次计算所用时间表的有效期仍明确拒绝。UTC 更新读取原文到期日期，验证日期/MJD、顺序、整数偏移及单秒正负变化，不修改 ERFA 全局表，也不手工延长有效期。先校验新 UTC 并重建当前 EOP 解释，再在一个 SQLite settings 事务中切换 UTC 与两种 EOP 的当前引用；保持 EOP 原下载时间。活动计算和显示继续捕获旧 Provider，下次打开／重算按新哈希确认。日期编辑器、时间轴和当前时间显示均使用对应的时间 Provider。

## 地球重力场

EGM2008 / EGM96 从 [ICGEM 官方模型目录](https://icgem.gfz.de/tom_longtime)下载完整 `.gfc` 文件，分别约 240 MiB / 5.4 MiB。固定来源原文 SHA-256 随实现核验，修改过的模型须导入外部项，不能冒充官方产品。重力文件采用 512 MiB 上限和 30 分钟总时限，网络、临时写入、后台解析及校验均可取消；成功后才切换当前引用，失败保留原文件。

这两个产品是静态模型，不按日失效。概览展示下载／导入时间、文件 Degree / Order、可用计算阶次、GM、参考半径、潮汐系统与实际固体潮 k20，并标注“无固定有效期”。文件信息页展示来源、完整原文哈希、路径和大小。重力场父级介绍可用模型；HPOP 配置选择具体资源，不设置类似 EOP 的优先级。完整 EGM2008 的 2190 阶、2159 次全部可计算，最高阶与最高次独立限幅。当前官方 ICGEM 导出将 2159 次以外显式补零至 2190 次，所以文件信息为 2190×2190；选择这些补零项不会增加模型精度或改变力值。EGM96 仍以其文件的 360×360 为上限。外部模型支持至 2190×2190，超出已验证范围明确拒绝。重力解释版本为 `normalized-gravity-2190-secular-julian-year-v4`，原文件不变；项目数据引用变化沿用打开／重算确认流程。重力收据 schema 3 保存 `solid_tide_k20` 与可空的显式长期变化模型，旧收据明确拒绝，须重新导入或下载；不迁移或补默认值。STK `.grv` 的 EarthIERSTideModel / LoveNumbers 与永久潮声明按[固体潮契约](solid_tides.md)解析，失败和取消保持原资源。

桌面首次启动进入工作台前必须准备完整的基础数据：按 UTC / 闰秒表、IERS C04、IERS finals2000A、EGM2008、DE440s 的顺序自动下载缺失项；未编译 CSPICE 的构建不要求 DE440s。先准备 UTC，再解释 EOP。准备窗口显示各项状态、实际接收字节、后台校验与安装进度；失败停止队列并提供重试或导入官方文件入口，取消退出本次启动。成功文件即时保留，后续启动只补齐缺失项；已有完整资源不重复获取。删除缓存文件后会重新准备，目录收据本身不算可用数据。图形烟雾测试与性能测量使用固定输入，不执行联网准备。定期更新在首次准备完成后才开始调度，静态重力与星历的定期检查仍默认关闭。

桌面新建 HPOP 配置默认选 EGM2008、21×21；引力页若选到尚未安装的 EGM2008 / EGM96，仍可通过数据中心后台自动补齐。页面显示连接、接收字节、校验和安装状态，提供取消与失败后重试；已安装时直接使用当前资源。其他数据任务正在运行时排队等待，取消排队或关闭窗口不会取消其他窗口启动的任务；关闭 HPOP 窗口取消本窗口启动且尚未完成的下载。下载失败不切换到内置示例，取消设置不提交模型或参数草稿；成功安装的文件保留在共享目录。主动开启按期更新后复用条件请求与调度机制。

桌面提供一个当前外部模型槽位 `custom`，支持静态 ICGEM `.gfc` 与 STK `.grv` 子集；Runtime 目录允许用独立 ID 管理多个自定义模型。解析器拒绝时变、潮汐及其他尚不支持的力模型块，不悄悄忽略。原文与模型 μ / 半径一同校验，计算层不联网补数据。

重力收据以 `gravity/<ID>` 存入 SQLite settings，记录获取方式与时间，完整文件保存在 `gravity/<sha256>.dat`。`DataCatalog.install_gravity()` 先复制和校验再发布；`gravity_models()` 查询收据，`load_gravity(ID)` 校验并加载不可变资源。`DataSnapshot.with_gravity_file(ID, path)` 用于显式装配。`project.prepare(catalog)` 只加载对象实际选择的重力资源，项目以 `gravity/<ID>` 记录原文哈希和解析配置；下载时间不参与计算身份。

```python
import orbitforge as of
from orbitforge import units as u

catalog = of.DataCatalog("local/data")
entry = catalog.install_gravity("EGM2008.gfc", model="egm2008")
data = of.DataSnapshot.load(eop_manifest="eop/manifest.json").with_gravity(
    entry.selection, entry.file)
project = of.Project("Gravity", of.Epoch(0), of.Epoch(600 * 10**9))
sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
    initial=of.orbits.Keplerian(of.Epoch(0), u.km(7000), 0, u.deg(30)),
    gravity_model=entry.selection, gravity_mu=entry.info.mu, gravity_degree=21))
analysis = project.prepare(data=data)
project.save("gravity.orbitforge", analysis=analysis)
```

此示例显式使用本地文件，EOP 必须覆盖示例时段。外部模型 GM 与轨道配置不符时需要重新选择模型、核对输入；不会仅通过接受新哈希静默改变初始根数的物理含义。系数更新沿用项目打开／重算的确认流程，运行中结果保持原资源。

## 太阳系星历与日月引力

左侧“太阳系星历”下管理 DE440s，右侧沿用概览、更新与计算、文件信息三页。来源为 [NAIF 官方 DE440s](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/de440s.bsp)，完整文件约 32 MiB，原文 SHA-256 为 `c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2`。提供手动下载、导入本地完整 BSP、条件更新、进度及取消；自动下载默认关闭，可主动开启。上限 64 MiB，总时限 30 分钟，下载和校验在后台完成。

本产品可计算覆盖为 **1850-01-01 00:00:00 TAI 至 2150-01-01 00:00:00 TAI**，是官方文件实际 SPK 覆盖内的保守区间。界面明确标注 TAI；历史或未来几何查询不依赖尚不存在的 UTC 闰秒表。将历元作为 UTC 输入、输出，或使用 EOP 时，仍分别遵守时间表和 EOP 的覆盖约束。DE440s 不按下载日期失效，也不允许通过 EOP 的外推设置延长星历覆盖。

安装过程校验原文哈希及完整目标链的连续覆盖，生成带时间转换常数的清单，再原子发布到 `ephemerides/<manifest SHA-256>/`。SQLite 收据 `ephemeris/de440s` 记录获取方式、获取时间与可用范围；`DataCatalog.ephemerides()` 读取元数据，`load_ephemeris("de440s")` 校验清单、文件与只读运行副本。HPOP 支持日月、七颗行星及冥王星；安装时检查全部目标链的连续覆盖。

“导入行星本体星历…”用于补齐火星、外行星及冥王星的 SRP 遮挡中心。导入前需有 DE440s；只提取本体相对系统质心的 J2000 片段，保留现有主星历和其他行星补充数据。文件信息页的“导入星历数据包…”接受显式 `manifest.json` 及同目录内核。两者均在后台运行，可取消，失败保留原数据；后续更新 DE440s 不丢失补充文件。输入与最终快照上限、覆盖和身份见[SPICE 契约](spice.md#行星本体补充星历)。文件信息从清单读取实际主 SPK 文件与哈希；历史片段快照可以按其覆盖范围使用，但不能满足首次启动的完整 DE440s 要求。HPOP 仍通过 Available / Assigned 分配遮挡体，不在力模型页管理安装状态。

HPOP 使用地心几何位置（无光行时/像差），通过专用 ICRF 轴适配器求差分引力。配套 GM 为太阳 `1.3271244004127942e20`、月球 `4.9028001184575496e12` m³/s²，来自 [NAIF DE440 常数](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/gm_de440.tpc)。每个天体也可手动设置 GM，与位置星历分别记录。这里不构造或重标记一个相对论 GCRF 状态；时标使用 CSPICE 的均匀时间转换模型。有关轴约定见 [NAIF Frames](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/frames.html)。

Python 显式装配本地文件的最小示例（点质量地球隔离 EOP 依赖；球谐地球还需覆盖全积分时段的 EOP）：

```python
import orbitforge as of
from orbitforge import units as u

catalog = of.DataCatalog("local/data")
entry = catalog.install_de440s("de440s.bsp")  # 显式本地安装，不隐式联网
data = of.DataSnapshot.builtin_time().with_ephemeris(
    entry.selection, entry.manifest, "local/ephemeris-runtime")
project = of.Project("Sun and Moon", of.Epoch(0), of.Epoch(86400 * 10**9))
sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
    initial=of.orbits.Keplerian(of.Epoch(0), u.km(42164), 0, u.deg(15)),
    gravity_degree=0, third_bodies=("sun", "moon")))
analysis = project.prepare(data=data)
project.save("sun-moon.orbitforge", analysis=analysis)
```

C++ 可用 `catalog.load_ephemeris("de440s")` 装配 `DataSnapshot::with_ephemeris()`，或设置 `HpopEnvironment::ephemeris` 后调用 `make_hpop()`；`HpopOptions::third_body(body)` 保存逐天体启用和 GM 来源（JPL DE / Cb file / User Specified）。Cb 常数是显式导入后捕获的配置值，位置仍来自统一管理的 DE440s，详见[第三体引力](third_body_gravity.md)。桌面、项目报告和任务序列 HPOP 滑行复用同一实现。

项目 v40 记录 `ephemeris/<选择 ID>` 的清单内容与解释哈希，解释包含几何位置、轴、GM 常数集、TAI 速度导数约定及 CSPICE 版本；下载时间和本机路径不参与身份。数据变化先确认，拒绝则停止打开或重算。覆盖检查包含初始状态到查询/场景的完整积分路径，检查内部空洞，不仅检查两个端点。运行中的分析持有不可变资源。

## 覆盖不足与有限外推

覆盖不足时列出场景需要的时段、每种当前数据的原始范围及外推上限，并提供“打开数据中心”和“取消”。用户下载、导入或修改设置后重试；不静默缩短场景，不绕过 UTC 有效期。

每类 EOP 独立保存外推策略，默认关闭。启用时默认 3 天，可设 1–30 天；这里一天是 86400 个连续 TAI 秒，分别从原文件首、末样本起算。30 天是本软件的工程上限，不是精度承诺。外推算法 `endpoint-linear-tai-v1` 将端点状态按其一阶变化率线性延伸：UT1−TAI、x/y、dX/dY 分别计算，变化率保持端点值，与原 Hermite 插值在端点连续可导。使用 UT1−TAI 和连续 TAI 时间避免跨闰秒跳变，不将闰秒表本身外推。

IERS 文件内已有预测属于文件数据；越过完整有效样本范围后的软件线性估算明确标作外推。原始 first/last、文件 SHA-256 与插值版本不变，`EopDataInfo.extrapolation_days` 单独记录本次策略。超过上限 1 ns 或 UTC 有效期均拒绝。原文件覆盖充分时不会启用包装器，因此不必要的设置变化不改变计算身份。

场景标题标记“EOP 外推”，场景属性列出实际使用的外推时段、方法和上限；每个外推样本来源带方法、上限和距端点秒数，轨迹记录整个分析区间。项目 v4 保存策略，并纳入解释配置哈希。关闭外推后重新打开越界场景会提示覆盖不足；改变实际使用的外推上限须确认，不能从旧项目自动恢复已撤销的许可。一次计算保留启动时的策略，不受中途设置修改影响。

项目仍须通过结构、校验和及 DATA/LOCK 一致性校验。经用户允许更换数据后标记未保存，普通保存才更新磁盘文件。底层 `resolve_eop(lock)` 保留对原始数据的精确读取；桌面正常打开使用当前目录及策略，不依赖旧缓存。严格复现和导出项目依赖包留作独立能力。

## 开发顺序

1. **已完成：** 数据引用值、规范哈希、Scenario 绑定、当前项目格式持久化和一致性验证。开发阶段不兼容旧项目格式，版本不匹配时明确拒绝。
2. **已完成：** SQLite 目录、文件去重、获取记录、按类型显示当前数据、旧目录迁移；打开或重算时检查数据变化并确认，哈希记录计算来源与外推策略。
3. **已完成：** finals2000A 全文件适配器、原文件导入、手动更新、HTTP 条件更新、取消与失败重试。C04 全量文件适配器、原文与已有清单导入也已完成。
4. **已完成：** 三种数据的自动检查／下载、按实例加载的 UTC Provider、UTC 更新后 EOP 重新解释和原子激活。
5. **已完成：** 静态重力场与 DE440s 的联网下载、目录管理、HPOP、API 和项目引用。空间天气和海潮资源已接入；后续增加地表辐射及其他潮汐资源；按实际需求增加结果复现、项目依赖包和缓存清理。

各增量都保留离线使用能力。验收重点是身份可核验、一次计算的数据一致、更新可取消，以及任何下载或应用失败都保留已有可用文件和活动场景。

## Python 示例

已经安装数据的目录可直接用于 `project.prepare(data=catalog)`；C++ 对应 `runtime::prepare(document, catalog)`。接口按整个项目时段读取当前来源、优先级和外推设置，数据变化仍需明确确认；同一次分析持有固定资源。按需提前调用 `catalog.snapshot(first, last)`，或使用 `earth_orientation=False` 只选择时间表。契约与错误处理见[分析 API](analysis_api.md)。

```python
import orbitforge as of

catalog = of.DataCatalog('local/shared-data')
entry = catalog.install_eop('snapshot/manifest.json')
catalog.record_acquisition(entry.lock_sha256, of.DataAcquisition.imported)
catalog.activate_eop(entry.lock_sha256)  # 安装后显式指定当前文件，旧分析不受影响
# 也可直接安装完整原文件，自动推导可计算范围，不需要手工准备清单：
# entry = catalog.import_finals('finals2000A.all',
#     'https://datacenter.iers.org/data/9/finals2000A.all', '2026-10-10')
project = of.load_project('mission.orbitforge')
provider = catalog.load_eop(entry.lock_sha256)
project.earth_orientation = provider.data_info  # 记录本次实际使用的数据
print(catalog.snapshots[0].acquired_at_unix_seconds)
# 直接加载文件仍使用显式策略；不读取目录设置：
limited = of.load_eop_snapshot(entry.manifest, extrapolation_days=3)
project.earth_orientation = limited.data_info
```

`ORBITFORGE_WITH_DATA_CATALOG` 默认关闭，图形桌面预设和标准 wheel 显式启用。关闭时 `data_catalog_available()` 返回 false，构造目录明确报告功能不可用；普通 headless 构建保持无 SQLite 依赖。


## 空间天气

空间天气单列显示，下载源为 [CelesTrak SW-All.csv](https://celestrak.org/SpaceData/SW-All.csv)，可导入 CSV、定宽 SpaceWeather、STK FXM 或 CSSI Predicts `.dat` 原文。文件上限 32 MiB，完整解析及哈希后原子发布到 `space-weather/<sha256>.dat`；失败或取消保留当前选择。目录收据记录来源、格式、原始首末日、获取方式及时间，获取时间不参与计算身份。

HPOP 文件模式在准备时捕获资源，后续更新不修改现有分析、报告和轨迹。重新准备时比较 `space-weather/<selection>` 数据引用，变化需沿用既有确认流程。日/三小时及月预报模式的可用范围还受模型滞后、历史 Ap、CSSI 中心平均所需前后各 40 日、缺值及 UTC 数据约束；界面原始记录日期不表示整段可计算。计算规则见[阻力](atmospheric_drag.md)。


## 海潮模型

“海潮模型”分组管理官方 FES2014bv1 与外部 POT。官方源使用 [GRGS 发布的 FES2014b.v1 with S1 势系数](https://grace.obs-mip.fr/dealiasing_and_tides/ocean-tides/)，100×100、36 个显式分潮。完整文件约 10.7 MiB；SHA-256 固定为 `74156304aca8f66ed1f67775de3d4a9d24ecb94abb47127ef40d5bb4630f719f`。未知或修改后的模型须使用外部选择 ID，不能冒用官方身份。

海潮默认关闭，不列为首次启动必需数据。第一次在 HPOP 引力页启用且缺少官方数据时自动下载；有其他任务则排队。取消排队不影响其他下载；关闭设置窗口只取消它自己启动的任务。失败显示原因并允许重试。外部模型通过数据中心导入，HPOP 只显示模型范围和分潮数，不显示“已安装”。海潮模型无 UTC 有效截止；实际计算仍受 EOP 覆盖约束。默认不自动检查更新，用户可显式开启。

Runtime 的 `DataCatalog::install_ocean_tides` 将文件复制到独占 staging，限制 128 MiB，完整校验后原子发布到 `ocean-tides/<sha256>.dat`，以 schema 1 收据记录选择 ID、模型名、内容哈希、来源 URL、阶次、分潮数、GM、参考半径和获取信息。失败/取消不改当前收据；加载再次检查内容和解释元数据。`ocean_tide_models()` / Python 同名属性列出当前资源，`load_ocean_tides(id)` 返回不可变模型。

准备项目时按需捕获资源；活动海潮的数据引用为 `ocean-tides/<id>`，不包含下载时间或本机路径。更新后仍使用既有数据确认流程，已经创建的分析保持原资源。完整算法、输入格式和未实现范围见[海潮契约](ocean_tides.md)。


重力目录的长期变化元数据包含参考历元 TAI 纳秒及排序后的归一化变化率（每儒略年），来源为 `.grv` 的 `SecularVariations` 块。数据中心和 HPOP 显示 TT 参考历元及变化率数量；缺少该块时显示未定义。目录重开保留完整元数据，加载时核对文件与收据的物理解释身份；损坏或不一致须重新导入。参考历元、变化率、GM、半径、阶次、潮汐系统和 k20 均进入配置哈希，导入时间与本机路径不参与。详见[长期变化契约](gravity_secular.md)。
