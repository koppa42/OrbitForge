# 大气阻力验证协议

模型范围见[阻力契约](../architecture/atmospheric_drag.md)。固定参考不链接 OrbitForge，普通测试只读取已提交参考，不联网重算。尚未完成 STK 同配置数值验收。

## 密度参考

`nrlmsise00.txt` 有 396 个原作者 C 实现 `gtd7d` 参考，原始提交固定为 `bc9a2feba4344e74201281e563332688a4d09cc3`（含 32.5 km 边界修复）；SI 输出、默认变化项、日 Ap。覆盖 3 个 UTC 日期（闰日及闰秒）、11 个高度（0…1000 km）、4 个纬度/太阳时位置（含两极）和 3 组天气。

`jacchia_roberts.txt` 有 576 个未修改 NASA GMAT 标量模型参考，提交为 `c9b78bd1c8499845cb78fde315e1f761b6b06f1c`。覆盖同样日期、16 个分段高度（90…2500 km，含 100/125/200/500 km 附近）、4 个几何条件和 3 组天气。API-only stubs 不执行时间/高度换算，标量入口直接接受 UTC JD−2430000、WGS84 大地纬度、椭球高、太阳方向；未调用的外层时间和测地函数会抛异常。参考不声明等同于 GMAT 全部外层传播配置。

当前 Core/Python 的 576 组 JR 验收读取 `jacchia_roberts_precise.txt`：12 列输入保持相同，≤125 km 保留原 GMAT 结果，>125 km 使用保留原方程温度相减的 80/100 位高精度结果。原 double 实现在高空会因温度相减损失精度，部分低太阳活动输入给出零密度；本实现通过直接指数比值修正，密度容限仍为 2e−10。原 `jacchia_roberts.txt` 留作可重现的来源输入，详见[偏导与高空稳定性验证](drag_differential_protocol.md)。

`nrlmsise00_ap.txt` 另含 1056 个同一原作者 C `gtd7d` 参考，覆盖 3 个日期、11 个高度、4 组几何和 8 组地磁输入：3 组原生日 Ap（含非 Kp 表节点的 15.25）、5 组历史 Ap（全零、恒定、最大值、上升和衰减磁暴）。历史模式显式设置原生 `SW[9]=-1`；测试完整密度及两个温度。历史均值包含非整数，不舍入到离散 Kp 表。参考 SHA-256 为 `da884507a21fc53dd8f5c0002bedb6270f289fbf88fbb254366b71eb219e9037`。

生成器为 `tools/validation/nrlmsise_reference.c`、`nrlmsise_ap_reference.c` 和 `jacchia_roberts_reference.cpp`；上游文件 URL 与原始字节 SHA-256 固定于 `density_sources.json`。`atmosphere_reference.py` 下载后校验字节、独立编译并在指定目录写候选文件，不能自动覆盖参考：

```powershell
# 在 MSVC developer shell 中；Linux 去掉 --msvc，使用 gcc/g++。
python tools/validation/atmosphere_reference.py out/validation/density-reference --msvc
```

Core/Python 对照密度相对容限 `2e-10`，NRL 温度绝对容限 `1e-7 K`，用于不同浮点运算顺序。另测非法日期、非法枚举、非有限参数、越界高度、Kp→Ap、极点/赤纬零/正午午夜及并发；这些测试不代表经验大气模型的物理精度。原生 Ap 测试额外检查两种输入互斥、JR 拒绝 Ap、七项长度与各项范围、显式 Ap 不读取非活动 Kp、输入数组所有权，以及日/历史模式交替和并发计算不串状态。Python 独立安装和 C++ SDK 消费端同时验证公开字段。历史 Ap 尚未用于 HPOP 轨道参考。

## 轨道参考

`drag.txt` 有 24 个独立 Orekit 13.1.2 / Hipparchus 4.0.2 `NRLMSISE00 + IsotropicDrag + DP853` 轨道和初始阻力加速度。固定 jars 及 SHA-256 复用 `tools/validation/orekit/dependencies.json`，生成器为 `OrbitForgeDragReference.java`。

参考刻意使用 TAI 作为模型的合成 UTC 时钟，初始时刻 2000-01-01 12:00:00；不混入真实闰秒/EOP 差异。WGS84 参考系绕 z 轴以 `7.292115e-5 rad/s` 转动，太阳按年周期移动并带 `0.409 rad` 倾斜。两组倾斜 LEO 初值、三组天气/物性、±3600/±6000 s，完整输入及每轴结果随行保存。Core 使用完整状态变换和 RKF78，独立参考使用 Orekit 的椭球高度、大气速度和 DP853。

DP853 最大步长 1 s，绝对位置/速度容差 `1e-8 m / 1e-11 m/s`，相对 `1e-14`；Core 最大步长 2.5 s、同样绝对/相对容差。位置每分量容限 0.5 mm、速度 0.5 µm/s；这是实现回归阈值，不是预报精度。本机参考收紧前使用 5 s 最大步长，收紧后最大位置差由 0.428 mm 降到 0.095 mm、速度差由 0.567 降到 0.125 µm/s；没有放宽测试阈值。

```powershell
$dragClasspath = (Get-ChildItem out/validation/orekit/jars/*.jar | ForEach-Object FullName) -join ';'
javac -cp $dragClasspath -d out/validation/drag tools/validation/orekit/OrbitForgeDragReference.java
java -cp "out/validation/drag;$dragClasspath" OrbitForgeDragReference
```

## 生命周期与接口

测试认证点质量+阻力仍需要 UTC/EOP/Sun，缺项和区间外查询先失败；视太阳需要额外 600 s 核覆盖，几何方向不冒充视方向；取消后 Session 可再次使用。用实际 ERFA/IERS/SPICE 2016 闰秒片段检查 C++ Session、Project、报告及 Python 状态一致；保存/重开保留参数及来源，修改 Cd/天气/密度模型/太阳方法改变新结果，旧 Analysis 保持不变。

项目 v36 固定样例独立组装，禁用参数草稿往返而不改变身份；NRL 主模型的非活动低空参数不计入计算身份。Qt 测试检查阻力开关、模型关联、面积质量比单位转换、错误定位、缺星历拒绝、取消/重开；真实 Windows 窗口检查 430/720 逻辑像素及至少两个 DPI 设置（日志记录实际 DPR），不重建编辑器、不丢失输入，操作按钮可达。


## 文件模式验收

`SpaceWeather.*` 覆盖手工可核算的 20:00 UTC 线性插值、JR/NRL 模型滞后、7 项历史 Ap、Kp 逐项转换后再平均、闰秒及跨日、内部缺日/缺字段与历史范围不足。`WeatherFiles.*` 使用官方格式示例及合成完整日期窗口，覆盖 CSV/定宽字段、缺值、重复日期、预报 Kp 中间值、段计数错误、取消/损坏后保留旧目录、原文哈希变化和旧快照不变。

轨迹验收以常量文件与手动相同模型/输入的严格一致性为基础，同时验证文件改变会影响轨迹、报告与 Session 一致、保存重开保留配置/数据引用，以及更新文件不会改变旧分析。`test_space_weather.py` 在安装包运行相同 API/持久化边界；`QuantityTest.HpopWeather*` 检查文件模式字段切换、草稿、窄窗、重开及取消；`DataCenterTest.WeatherDownload*` 经本地 HTTP 服务检查后台发布与失败回退。官方实时文件验证另记录下载时全文哈希及结果，不作为离线 CI 的联网依赖。

磁暴跳变回归在 15→400 Ap 边界比较 RKF78 最大步长 60/7/1 s、RKV89 / Bulirsch–Stoer 最大步长 60/7 s 与独立固定 RK4 0.5 s，正反向 300 s 弧的末端位置差须小于 0.1 mm。该容限用于积分收敛，不代表真实大气密度误差。

## 高度近似

`AtmosphericDrag.ApproximateAltitude*` 使用独立 60 位十进制射线与 WGS84 椭球交点作为高度参考：先解 `(t*x/a)²+(t*y/a)²+(t*z/b)²=1`，再计算 `|r−t*r|`。固定点涵盖中纬度、南半球和近极点，容差 3 nm（地球半径量级 double 舍入）；验证经纬度和精确大地高没有被替换。JR/NRLMSISE 输入同一参考高度后，密度相对差需小于 `1e-12`；原作者密度网格继续独立验证模型本身。

赤道、两极及 0/90/400/1000 km 检查有限极限和域边界；在近似高度刚过 90 km、精确高度仍低于 90 km 的点，检查零宽硬切换使用正确模型；100 m 拼接区中点检查两模型等权密度；中点 Cartesian 舍入的高度容差按权重导数上界 `1.5/100 m` 传播到密度容差。非有限输入、地心及域外高度继续拒绝。

Python 检查开关、返回的实际密度高度及加速度变化；C++/Python 的保存重开、分析/报告身份、旧快照不变和 Qt 草稿/取消/重开检查均包含该开关。此处验证的是公开公式和软件路由，尚未取得 STK 同配置轨迹输出。

## 自然三次样条与密度链路

`space_weather_spline.txt` 由 `tools/validation/space_weather_spline_reference.py` 调用固定 SciPy 1.16.3 的 [CubicSpline](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.CubicSpline.html)（`CubicSpline(bc_type="natural", extrapolate=False)`）生成，不读取 OrbitForge 算法或输出。112 个三小时输入节点包含独立的 Ap/Kp 序列，1112 个取样覆盖节点、节点后 1 ns、节点前 1 ns、中间值、跨 2016 正闰秒及首末端点。逐项绝对容限 Ap `5e-12`、Kp `5e-13`，覆盖独立三对角求解器和全局/局部坐标的浮点舍入。

66 个密度样例将 SciPy 的天气输出传给未修改 NASA GMAT `c9b78bd1` 标量 JR；几何、高度、UTC JD、150 sfu 日/平均通量及 Kp 显式输入。适配器为 `jacchia_roberts_weather_reference.cpp`，使用已有 `gmat_jr_stubs`；上游原文的两个 SHA-256 沿用 `density_sources.json`。选 Ap 时，参考侧用标准 28 点表和 NumPy 插值转为 Kp。Core 从同一原始日记录重新计算天气和密度，密度相对容限沿用 `2e-10`，不放宽原有标准。

复现时先用 `atmosphere_reference.py` 获取并认证原始 GMAT 文件；在 MSVC developer shell 中编译适配器（Linux 可用 g++ 对应选项）：

```powershell
cl /nologo /O2 /EHsc /std:c++20 /I<上游目录> /Itools/validation/gmat_jr_stubs tools/validation/jacchia_roberts_weather_reference.cpp /Fe:<输出目录>/weather_reference.exe
python tools/validation/space_weather_spline_reference.py <候选文件> <输出目录>/weather_reference.exe
```

参考只按 LF 换行保存。额外测试缺测分段、端点不外推、内部极值越界、Ap/Kp 来源独立、NRL 原生历史和有效身份、禁用草稿、当前格式未知枚举拒绝、Python 安装、SDK 保存重开及桌面窄窗/DPI/键盘/取消。STK 的节点落点和端点处理未由公开文档确定，仍单列待验收，不用 SciPy/GMAT 对照代替 STK 同配置实测。

该参考文件的 LF 字节 SHA-256 为 `92cee02d59e8eeee3244ac2af3ab2f138307777d8deb132e13957c7e86d64fce`，重算结果逐字节一致。


## FXM 调整通量与文件传播

`tools/validation/fxm_weather_reference.py` 不导入 OrbitForge，用 NumPy 2.3.3 和 SciPy 1.16.3 `interp1d` 对独立计算的节点通量进行插值，并将预期天气输入交给原始 NASA GMAT JR 适配器。适配器、原始源码版本与散列沿用 `jacchia_roberts_weather_reference.cpp` / `density_sources.json`。复现方式：

```text
python tools/validation/fxm_weather_reference.py OUTPUT_DIR GMAT_DRIVER
```

输出 `weather_fxm.fxm` 和 `weather_fxm.txt`。参考使用显式合成的 0.97–1.035 AU 节点距离，刻意加快变化以检测改正次序和滞后错误；它不是实际太阳星历。74 组 JR / NRL 天气样本覆盖 20:00 UTC 前后 1 ns、半日、2016 年正闰秒和观测/预测切换；37 组 JR 密度覆盖 100、126、400、1000 km。通量容限 `3e-11 sfu` 包含独立插值时间坐标的浮点舍入；GMAT 相对密度容限 `2e-10` 沿用现有 JR 对照。

C++ 集成测试另外用真实 DE440 星历，将 FXM 路径与显式转换的 observed CSV 路径比较，覆盖 JR、NRL、低空模型准备、视太阳方向、项目保存/重开与数据变更确认；位置容限 `1e-5 m`、速度 `1e-8 m/s`。此项验证接口组合与转换路由，不是独立轨道真值。格式损坏、预测重叠/缺字段、错误 Kp/Q、取消导入、无距离或内部覆盖缺口均有失败路径测试。Python 安装后对同一参考执行 74 组取样并检查可选回调及目录重开。

尚未获得 STK 本机输出，不能据此声称 FXM 改正与 STK 位级一致；FXM 的日预测没有补成三小时历史，CSSI Predicts 月预报采用下述独立契约与验证。


## 保持三小时均值的插值

`tools/validation/space_weather_mean_reference.py` 使用固定 NumPy 2.3.3 / SciPy 1.16.3，将原始窗口值累积后调用 `CubicSpline(..., bc_type="natural").derivative()`，再用独立多项式 API 求极值并缩放。生成器不导入 OrbitForge，不使用生产求解器或其输出；密度参考调用上述未修改 GMAT JR。复现命令：

```text
python tools/validation/space_weather_mean_reference.py OUTPUT GMAT_DRIVER
```

固定参考含 112 对独立 Ap/Kp 输入、1314 个逐点值、218 个窗口的五点 Gauss–Legendre 积分和 70 个 GMAT 密度结果。覆盖平稳变化、Ap=0/400、Kp=0/9、磁暴跳变、16 个受限制的窗口、来源独立缺测、单窗口和双窗口序列、UTC 边界前后 1 ns，以及 2016 正闰秒的 10801 s 窗口。逐点绝对容限 Ap `1e-10`、Kp `1e-11`，计入 SciPy 对约 1e4 累积量求导的舍入；窗口均值绝对容限 `1e-10`，计入积分节点映射到整纳秒的误差；GMAT 密度相对容限仍为 `2e-10`。

C++ 另测缺日期/月预报断档、窗口外拒绝、不可变数据、NRL 原生历史不变、有效计算身份及保存草稿。真实 DE440/EOP 场景对两种插值分别作 ±6 h RKF78 最大步长 60/7 s 收敛检查，位置/速度每分量容限 1 mm / 1e-6 m/s，并验证保存重开精确重现。此项是数值积分收敛，不是独立轨迹真值。Python 从 CSV 重建独立输入后复核全部 1314 点与 218 个积分，并验证高层设置保存；安装 SDK 与 Qt 窄窗/DPI、四项顺序、键盘切换、重开和取消同步验收。

参考 SHA-256（LF）：`d85e283b597361fe3180c979fbfa373ba0e17322cf2e40d2da528ea28cb64ac1`。STK 公开约束已实现；缺少 STK 本机参考时，不能以这些独立算法对照宣称内部边界处理完全一致。

## 月节点天气和 CSSI Predicts

`tools/validation/monthly_weather_reference.py` 固定 NumPy 2.3.3 / SciPy 1.16.3，不导入 OrbitForge。它用 `interp1d` 计算月节点线性曲线、`numpy.convolve` 计算 81 日中心均值，独立组装七项历史，随后把预期天气交给原始 GMAT JR 适配器。时间节点以独立整数 UTC→TAI 映射构造；测试区间分别采用 36 / 37 s 偏移。复现命令：

```text
python tools/validation/monthly_weather_reference.py OUTPUT_DIR GMAT_DRIVER
```

输出 `weather_cssi.dat`、`weather_monthly.csv` 与 `weather_monthly.txt`。输入包括 18 个月中 CSSI 名义 F10.7 / Ap 节点，以及含观测末段和月初预报的 CelesTrak CSV；三小时预报槽保持为空，CSV 的 Ap / Kp 刻意独立。1056 组逐点参考覆盖两个文件、JR / NRL、四种更新、两种来源、2016 闰日/正闰秒、月节点前后 1 ns、滞后节点与观测到预报的连接。32 组 GMAT JR 密度覆盖 100、126、400、1000 km。

通量及 Ap / 历史绝对容限 `3e-10`，Kp `3e-11`，计入独立插值坐标和 81 次求和的舍入差；GMAT 相对密度容限 `2e-10`。这验证公开计算约定的实现，不代表已有 STK 输出对照。CSSI 第 15 日、Ap 来源及 F10.7 20:00 线性插值有官方依据，直接月预报的中心平均和边界细节仍待 STK 验收。

C++ 另测损坏头部/列数/数量/顺序/非有限数、缺月、指数或均值缺测、中心平均的前后 40 日余量、最后地磁节点后 1 ns 的整段拒绝、原文字节/取消导入与目录重开。真实 DE440/EOP 场景对两个文件及两个密度模型作 ±6 h RKF 7(8) 连续滑行最大步长 30/7 s 对照，局部相对容差 `1e-14`、位置/速度绝对容差 `1e-8 m / 1e-11 m/s`，位置/速度每分量验收线保持 `1 mm / 1e-6 m/s`；再验证项目保存重开、天气更换确认及旧分析不变。此项是步长收敛和数据路由检查，不是独立轨迹真值。

安装后的 Python 复核全部 1056 组并检查目录元数据及失败后保留原数据；安装 SDK 用常值 CSSI 验证公共取样入口及平均值。Qt 检查切换 CSSI 后 Ap 自动选中且不可编辑、切回普通文件可选择 Kp、取消与重开保留已应用状态；真实 Windows 窗口检查 430/720 逻辑像素及 DPR 1/1.5。

固定文件 SHA-256（原始 `.dat`；CSV/TXT 为 LF）：

- `weather_cssi.dat`: `d3c3ab1ee4654c2a5e14a7020d6d30b2f0d1247c7fc0b6fbd188b6ad598a99ea`
- `weather_monthly.csv`: `cd0add4a31dea4e1c397af6849d10b871141c02a6121f13ad7861049fb15a890`
- `weather_monthly.txt`: `ca82c8aa82186544d85580e7c3944de7e646dcbf7827685125f71ec8ae3006e3`

七阶推进在上述月预报场景中采用更严格的局部容差。此前 `1e-13`、`1e-6 m / 1e-9 m/s` 配置的 60/7 s 最大速度差为 `1.2452e-6 m/s`，超出既定验收线；复算收紧局部容差后，最大位置/速度差为 `3.37035e-4 m / 4.52835e-7 m/s`，通过原标准，不放宽全局位置/速度验收线。局部容差本身不保证长弧段的全局精度。

普通项目接入连续科学星历后不再每 300 s 重启积分；原 60/7 s 配置在 NRL 月预报例超出上述全局线。独立安装的 Python API 另作 60/30/7 s 收敛测量，缩小步长并收紧绝对容差后保留原 `1 mm / 1e-6 m/s` 验收线；当前原生回归使用 30/7 s。该变化调整验证配置，不改变产品默认容差，也不放宽全局误差门槛。

当前连续滑行 30/7 s 四组回归的最大位置/速度分量差为 `0.0004465547390282154 m / 5.793299351353198e-07 m/s`，通过原门槛，记录见 `out/validation/hpop-project-interpolation-20261017/monthly-final.xml`。
