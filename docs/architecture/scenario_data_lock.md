# Scenario 数据引用与哈希

项目 v40 支持仅 UTC 的来源记录：有 EOP 时使用其完整依赖锁，否则使用独立 UTC 锁，无依赖时为空锁。文件重力以 `gravity/<选择 ID>`、太阳系星历以 `ephemeris/<选择 ID>`、空间天气以 `space-weather/<选择 ID>`、海潮以 `ocean-tides/<选择 ID>` 合并到锁中；记录文件或清单哈希与解释版本，不含下载时间或本机路径。`save(..., analysis=...)` 另存物理输入与所选数据锁的哈希，准备记录是否过期由当前输入和数据身份比较得出；它不替代单次查询结果各自的依赖记录。详见[分析 API](analysis_api.md)。

每个计算中的 Scenario 持有不可变 `DataLock`，用于记录本次实际使用的数据内容与解释配置。项目保存上次计算的数据引用和哈希，桌面打开或重新计算时检查当前数据；身份或实际使用的外推策略变化须用户确认，不要求安装旧快照。当前实现覆盖 EOP、UTC/TAI 时间、外部静态重力、HPOP 太阳系星历、空间天气与海潮。星历解释包含 DE440 几何位置、轴和 GM 常数约定及 CSPICE 版本；手动 GM 是对象物理输入。禁用天体的 GM 草稿及未使用的星历选择不改变计算身份。清单规范编码保留；启用外推使用独立的 EOP 解释版本。

## 引用字段

| 字段 | 含义 |
| --- | --- |
| `role` | 依赖用途；当前为 `earth-orientation`、`utc-time`、`gravity/<id>`、`ephemeris/<id>`、`space-weather/<id>`、`ocean-tides/<id>`，同一锁内不能重复 |
| `snapshot_id` | 固定快照 ID，不能写 `latest` 之类的浮动别名 |
| `content_sha256` | Provider 实际读取的数据文件原始字节 SHA-256，64 位小写十六进制 |
| `interpretation_version` | 数据读取与解释规则版本 |
| `configuration_sha256` | 有效期、坐标系、插值及上游依赖等解释配置的 SHA-256 |

`DataLock.sha256` 对整个有序引用清单计算 SHA-256。它是数据依赖集合的身份，不是整个项目或计算结果的哈希；重命名对象、修改轨道或播放时刻均不改变数据锁。未来结果缓存仍需另外包含计算输入、模型和算法版本。

本地路径、下载时间和来源 URL 不参与锁，来源 URL 仍保存在项目 DATA 中用于溯源。同一快照的同内容镜像不会改变锁；快照 ID 本身属于身份，另起 ID 会产生不同的锁，即使文件内容相同。数据中心可复用同一内容文件，但不能只依据文件哈希把不同解释配置合并。

## 规范编码 v1

编码不使用 JSON。字符串采用 `u32` 小端字节长度加字符串原始字节，不包含 NUL 或结尾换行；项目中的字符串必须为合法 UTF-8。所有 SHA-256 均以小写十六进制字符串写入。引用按 ASCII `role` 升序排列，最多 64 项。

清单字节顺序：

1. 字符串 `orbitforge-data-lock-v1`。
2. 引用数量 `u32`。
3. 每个引用的五个字符串，顺序与上表相同。

对这些字节计算总 SHA-256；总哈希不包含自身。`canonical_bytes()` 暴露同一编码，供其他实现独立验证。空清单明确表示尚未绑定数据，其 SHA-256 为 `81700a60bcada79003d1ea38a39cbea749f3997c1cdea22737be0bee6b7aeab1`。

`utc-time` 使用解释版本 `orbitforge-utc-tai-v1`。其配置依次编码该版本字符串、UTC 有效 MJD 起点 `u32`、终点 `u32`，再取 SHA-256；文件内容哈希对应时间数据原文。

禁止外推时，`earth-orientation` 使用解释版本 `orbitforge-eop-reader-v1`。其配置依次编码该版本字符串、参考系字符串、插值版本字符串、覆盖起止 TAI 纳秒各 `i64`（补码小端）、**仅含上述时间引用的 DataLock 总哈希字符串**，再取 SHA-256。因此时间内容、版本 ID 或有效期改变，即使 EOP 文件不变，也会改变 EOP 配置及整个数据锁。EOP 内容哈希对应实际读取的快照文件；如果该文件是源文件的截取子集，不能拿完整下载源文件的哈希替代。

实际计算需要外推时，解释版本改为 `orbitforge-eop-reader-v2`。配置编码为版本、参考系、原插值版本、原始覆盖起止 TAI 纳秒、字符串 `endpoint-linear-tai-v1`、最大外推天数 `u32`（1–30）、仅含时间引用的 DataLock 总哈希字符串；再取 SHA-256。文件内容哈希不变。策略与原始数据分离，不能通过修改 first/last 冒充新增样本。实际外推时段可由项目分析区间与原始覆盖计算。

解释或规范编码发生不兼容变化时必须使用新版本，同步更新项目格式并拒绝旧文件，不能修改版本的既有含义。SHA-256 核验内容及身份一致性，不认证数据发布者。

## 场景和项目行为

| 操作 | 数据锁行为 |
| --- | --- |
| 新建纯 GCRF 无界面 Scenario / 未绑定 Project | 空锁；不自动访问数据中心或网络 |
| 桌面新建场景 | 选择覆盖时段的当前 EOP，记录实际数据身份 |
| 重命名对象、查看结果 | 不改变本次计算的数据 |
| 修改轨道、复制、删除对象 | 候选场景检查当前数据；身份变化经用户确认后一起重建所有对象的显示轨迹；保留未编辑对象的数值 Session |
| `Scenario.with_data_lock(lock)` | 相同锁不增修订；不同锁返回新 Scenario 修订，旧读者及 Session 保持有效 |
| 下载或导入数据 | 更新目录的当前文件；已运行的结果不立即改变，下次打开或计算时检查变化并请求确认 |
| 保存 | 核对运行中锁与当前 Provider 信息一致；写入当前项目格式的 LOCK 段 |
| 打开当前格式 | 校验清单 SHA-256 和活动来源/LOCK 一致性；身份变化时请求确认；同意后重建并标记未保存，取消则不打开 |
| 打开非当前格式 | 解析内容前拒绝，提示实际与支持版本，保留原场景和文件 |

当前格式中，即使没有任何依赖，LOCK 也必须存在。缺失 LOCK、根哈希错误、活动来源（优先 DATA，否则 TIME，否则空锁）与 LOCK 不一致仍表示文件损坏并拒绝读取。文件有效但所记录旧数据不在本机不构成错误；如果当前没有覆盖完整时段的数据，且已允许的有限外推也不足，则展示覆盖缺口和数据中心入口，保留原场景。

`DataLock` 是身份值，不自行加载文件或下载数据。无界面 `Project.to_scenario()` 保存这份信息，当前二体/J2 的 GCRF 传播不需要 EOP；调用者真正使用外部数据时应记录相应 Provider。`runtime::DataCatalog::resolve_eop()` 仍提供显式的精确解析 API，桌面正常打开不调用它。严格复现属于按需增加的能力，普通场景没有固定历史版本的开关。

## API

C++ 提供 `DataReference`、`DataLock`、`eop_data_lock`、`Project::data_lock()` 和 `Scenario::with_data_lock()`。Python 的引用和锁属性只读，`references` 返回值副本：

```python
import hashlib
import orbitforge as of

project = of.load_project('mission.orbitforge')
lock = project.data_lock
assert hashlib.sha256(lock.canonical_bytes()).hexdigest() == lock.sha256
assert project.to_scenario().data_lock.sha256 == lock.sha256
for ref in lock.references:
    print(ref.role, ref.snapshot_id, ref.content_sha256)
```

场景属性显示“本次计算数据 SHA-256”，可以复制用于对照。时间 Provider 支持内置只读表及独立加载的 UTC 文件，每个实例保持自身内容与有效期不变；数据中心更新不会替换运行中的实例。其他数据类型和依赖文件打包尚未接入。
