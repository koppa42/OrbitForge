# CSPICE 星历与时间接口

Runtime 的 `ephemeris_available()` 查询构建能力，`load_spice_snapshot(manifest, workspace)` 加载显式本地快照并返回只读 `IEphemerisProvider`。Python 使用相同入口，接受字符串或 `pathlib.Path`。禁用 SPICE 的构建保留能力查询和工厂声明，工厂明确报错。Core/Application 不依赖 CSPICE 类型。

## 时间与状态

`to_tdb(Epoch)` / `from_tdb(TdbTime)` 调用 CSPICE N0067 `unitim`。Epoch 仍为以 **2000-01-01 12:00:00 TAI** 为零点的 int64 纳秒；`TdbTime.seconds_past_j2000` 是以 **2000-01-01 12:00:00 TDB** 为零点的双精度秒。浮点查询时间无法保留 Epoch 的所有纳秒，返回状态仍保留调用者的原始 Epoch。

样例窗口内 TAI/TDB 往返要求 ≤1 µs，输入范围与比较方法见[验证协议](../validation/p0_spice_protocol.md)。远离零点时浮点量化增大，不承诺全 int64 范围相同精度。CSPICE 简化的 TDB−TT 模型也不是纳秒精度模型；本项目另与 ERFA 地心模型比较，在选定日期要求差异 ≤50 µs。这个模型容限与浮点往返容限含义不同。

LSK 只用于上述均匀时间转换所需常数。UTC↔TAI 继续使用独立版本的 IERS 时间 Provider，不用 LSK 的闰秒表替代它，也不调用 SPICE UTC 字符串解析。

`geometric_state(target, observer, epoch)` 及 `geometric_states(target, observer, epochs)` 使用显式 int32 NAIF 编号、`J2000`、`abcorr=NONE`。返回 `EphemerisState`：

| 字段 | 含义 |
|---|---|
| `epoch` / `tdb` | 原始 TAI Epoch / 实际传入 SPICE 的 TDB 秒 |
| `target_naif_id` / `observer_naif_id` | 目标与相对原点的 NAIF 编号 |
| `frame` | `SPICE_J2000` |
| `position_m` | 相对位置，米 |
| `velocity_mps` / `velocity_time_scale` | 相对速度，米/秒；导数时间尺度明确为 `TDB` |
| `source_id` | 清单 SHA256、工具包、轴标签、像差选项和速度时间尺度 |

DE440 数据使用 ICRF 对齐的轴。此接口保留 SPICE 标签，尚未实现 BCRS/GCRS 的完整相对论转换，不能将结果直接改标签作为 GCRF `CartesianState` 输入地球旋转接口。原始速度保持每 TDB 秒；`tdb_seconds_per_tai_second(epoch)` 依据快照自身 LSK 给出解析 dTDB/dTAI，HPOP 的几何状态适配器显式转换速度导数用于[相对论力](relativity.md)，这不构成坐标系转换。几何状态接口不应用光行时/光行差；另有下述受限太阳接口。尚未提供任意 FK/PCK 参考系或地面站观测量。

## 接收型太阳向量

`apparent_sun_from_earth(epoch)` 返回地心 LT+S 太阳向量。`apparent_sun_from_spacecraft(epoch, position_earth_m, velocity_earth_tai_mps)` 返回从移动卫星指向太阳的接收型 LT+S 向量，单位米，ICRF 对齐轴。后者明确接受地心位置和 **每 TAI 秒** 的相对速度，通过 LSK 时间模型导数换算速率，再与 Earth/SSB 的 TDB 状态组合；只用于牛顿力环境，不将 `EphemerisState` 重命名为 GCRF 状态。位置/速度必须有限，SSB 观测者速度须小于光速。

地心方法继续使用 600 秒历史余量。卫星方法用实际几何距离的一次光行时估计，查询发光时刻前检查快照策略边界，再应用恒星光行差；不假定卫星始终靠近地球。`validate_spacecraft_apparent_sun_interval(first,last)` 检查接收区间的 Earth→EMB→SSB 链及快照起点到 last 的 Sun→SSB 连续历史；发光时刻是否仍在该范围内还须在每次求值时确认。详见[太阳辐射压](solar_radiation_pressure.md)。

## 快照清单与覆盖

清单最多 64 KiB，schema 1 仅接受以下字段；额外字段、浮点版本/历元值和不支持的版本均拒绝：

| 字段 | 约定 |
|---|---|
| `schema_version` | 整数字面量 `1` |
| `snapshot_id` / `model` | 非空版本标识 / 模型说明 |
| `first_tai_ns` / `last_tai_ns` | int64 TAI 纳秒，首项小于末项；两端均包含 |
| `kernels` | 有序数组，最多 16 项，恰好一个 LSK、至少一个 SPK |

每个内核严格包含 `file`、`kind`、`sha256`、`source_url`、`source_sha256`、`producer`。`file` 必须为同目录文件名，解析后的路径仍须位于该目录；重复路径拒绝。类型为 `LSK`、`SPK` 或 `SPK_BODY_CENTRES`，并核对文件实际结构。最后一种角色仍是二进制 SPK，但逐段验证仅包含一个物理行星中心（499、599、699、799、899、999），观测中心为自身系统质心（4–9），参考轴为 J2000。文件名须为 `centre-<NAIF ID>.bsp`，拒绝夹带太阳、地球或其他目标的片段。两个 SHA256 均为 64 位小写十六进制；`sha256` 检验交付文件，`source_sha256` 记录原始来源。来源字段和散列不构成身份认证。

内核总字节数最多 64 MiB。读取到内存后先校验散列，再用同一字节缓冲区创建运行副本。加载顺序按清单保留，重叠 SPK 的优先级因此属于快照契约。清单原始字节另计算 SHA256；`data_info` 保留完整顺序和来源。

清单窗口是项目允许的查询范围，实际 SPK 覆盖按目标/观测者链在查询时检查。加载成功不表示所有编号在整个窗口内都有数据。窗口外和实际覆盖不足均报错，不外推、不自动寻找其他数据。TAI/TDB 转换也限制在同一清单窗口。空批次返回空结果；批次中任一点失败时整次调用失败，不返回部分结果。

LSK 用 `ldpool` 加载，拒绝 `KERNELS_TO_LOAD`、`PATH_SYMBOLS`、`PATH_VALUES`，禁止元内核间接加载。`DELTET/DELTA_T_A`、`K`、`EB`、`M` 必须为有限数值且基数分别为 1、1、1、2，TT−TAI 必须为 32.184 s。该检查同时防止 N0067 在不完整时间常数下继承上一个快照的内部缓存。

## 文件归属与并发

工作目录由调用者显式提供。每个 Provider 在其中创建唯一子目录，持有自己的内核运行副本；原始快照随后移动或修改不会改变已加载对象。Provider 销毁时先关闭本实例使用的 CSPICE 内核，再删除自己创建的文件及空子目录，保留调用者的工作目录。加载失败也执行同样的有限清理。

Windows 实现持有拒绝写入/删除的只读文件句柄，保护延迟读取的 SPK 副本。CSPICE 的文件接口仍使用窄字符路径，运行副本绝对路径限 255 字节；推荐短的 ASCII 工作目录。尚未验证任意 Unicode 工作目录。Linux 构建尚未验证，POSIX 路径也尚无等效的排他文件保护，不能据 Windows 结果宣称跨平台相同保障。

同一链接实例内的全部 CSPICE 调用共用一个进程内互斥锁，包括首次错误配置、类型检查、切换内核、时间/状态求值、错误提取、reset 和 kclear。一个批次只获取一次锁；不同 Provider 交替使用时清空并按各自清单重新加载。加载/查询出错后清理错误状态，后续合法调用仍可使用。

Python 计算和加载释放 GIL，再由此锁串行管理 CSPICE。这允许其他 Python 工作继续运行，但没有让多个 CSPICE 批次并行执行。返回状态、批次及 Python 元数据副本拥有自身存储。C++ 消费端不得绕过封装调用同一 CSPICE 实例；外部直接调用不受本锁约束。当前尚无多进程吞吐或大内核切换性能结论。

## 错误与装配

`EphemerisDataError` 表示加载、格式、散列或内核常数问题；`EphemerisCoverageError` 表示清单窗口或实际 SPK 覆盖不足；二者在 Python 中继承 `ValueError`。其他 SPICE 查询问题为 `EphemerisQueryError`，Python 继承 `RuntimeError`。非有限 TDB 输入为 `ValueError`。错误消息保留 SPICE 短/长说明，库不会直接终止应用。

`ORBITFORGE_WITH_SPICE` 默认关闭；`astronomy` 预设和标准 wheel 显式开启。CSPICE N0067 通过固定 vcpkg overlay 静态链接，SDK 消费端另外提供对应开发包，wheel 包含私有实例与许可证。仓库和两类交付均附带一周历史样例，当前不能直接查询今天的星历。

## 依据

- [NAIF CSPICE 常见问题与并发限制](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/problems.html)。
- [unitim：均匀时间尺度](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/unitim_c.html)。
- [spkez：状态、单位与像差选项](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/spkez_c.html)。
- [SPICE 参考系约定](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/frames.html)。
- [ldpool：仅加载文本内核变量](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/cspice/ldpool_c.html)。
- [NAIF 数据和派生内核规则](https://naif.jpl.nasa.gov/naif/rules.html)。

## 行星本体补充星历

`DataCatalog::install_planetary_centres(source, source_url)` 导入最多 2 GiB 的本地二进制 SPK，必须已有 DE440s。输入先流式复制并散列，在 CSPICE 全局锁内用 `spksub` 原样提取上述本体相对系统质心的片段，保留原片段标识与系数，不重拟合。仅替换本次导入的行星；其他补充文件保留。导出清单记录原文件来源与 SHA-256、派生文件 SHA-256 和生成者。最终快照仍受 64 MiB 总上限约束。

`install_ephemeris_snapshot(manifest)` 导入完整的显式数据包，须同时通过 Provider 和 HPOP 的 DE440s 主星历约束、全部引力目标链连续覆盖认证。补充文件自身覆盖可小于主清单窗口，HPOP 按所选遮挡体及实际传播区间检查整条链与内部缺口。普通 DE440s 保持 v3 解释身份；存在补充文件时采用 `body-centres=filtered-spk-v1` 的 v4 身份。

两类导入均先准备独立副本，再验证、原子发布。取消、损坏或准备期间目录收据被另一写入者更新时保留当前有效数据；后一种情况明确要求基于最新快照重试。已有分析继续持有原快照。更新完整 DE440s 时保留本体补充文件。原始 `SPK` 角色仍仅接收 DE440s 来源；不能仅靠改写角色使任意主星历绕过限制。来源哈希只追踪内容，不是来源真实性认证。
