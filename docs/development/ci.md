# CI 与版本发布

所有改动先推送主题分支，再通过 Pull Request 进入 `main`；禁止直接推送 `main`。合并前更新到最新目标分支，确认当前提交的 **CI passed** 成功，再由维护者确认合并。提交或推送分支不代表授权合并。

## 触发与分层

| 触发 | 执行范围 |
| --- | --- |
| 代码、测试、CI、依赖或资源 PR | Windows / Linux 编译与测试、Python 3.11–3.15 API 测试；不构建发布包 |
| 纯文档 PR / 纯文档合并到 `main` | 工作流静态检查、版本一致性及工具测试；跳过原生构建和 Python 矩阵 |
| 其他 `main` 更新 | 两平台 C++、桌面、Linux sanitizer 和基准 Python 3.13 测试；不重复其他 Python 版本，不构建发布包 |
| 手动运行 | 与代码 PR 相同的完整测试，可用于复核当前 `main` 或排查问题；不构建发布包 |
| `vX.Y.Z` 标签 | 完整测试、发布包构建、安装与迁移验证；全部成功后创建 GitHub Release |

纯文档的范围限于根目录 `README.md`、`AGENTS.md` 和 `docs/` 下的 Markdown 文件。PR 分类对比模拟合并提交与其目标分支父提交；`main` 分类对比 push 事件的 `before..after`，校验当前检出等于 `after` 且推送为快进。两者均包含整个变更范围及重命名前后的路径，不能仅凭最后一次提交或文件扩展名跳过测试。其他路径、空差异、历史缺失或无法确定的差异，PR 保留完整测试，`main` 保留基准测试。

测试需要先编译 C++ 和 Python 原生扩展，但不需要创建可分发的安装包。普通 PR 和 `main` 不生成桌面 ZIP、SDK 发布归档或 wheel，不创建 Release，也不上传 `*-release-assets`。Python 测试按 `pyproject.toml` 的相同功能配置直接使用 CMake 编译扩展，将扩展及配套 Python 文件放入独立虚拟环境，再运行 API 测试与示例。wheel 元数据、打包布局和安装行为另由标签流程验证。

PR、`main` 与发布安装验证通过 `tools/ci/validation_inputs.py` 整理相同的独立测试目录，包含 Python 测试、九个示例、SDK 消费端、固定参考值、项目文件样例及测试使用的 EOP / 闰秒 / SPICE 片段数据。测试目录保留相对路径，但不复制 Python 包源码或 C++ 实现；计算能力必须来自被测安装。普通 Python CI 也从此目录运行，提前发现发布验证输入遗漏。

PR 的 `linux-python-dependencies` 和 `windows-python-dependencies` 仅供其他 Python 版本复用已编译依赖，内容不包括 OrbitForge 安装包。诊断日志另行保留。标签流程还传递 `linux-delivery`，供没有源码检出的新 runner 验证 SDK 和 wheel，然后整理发布附件。

PR 使用 GitHub 默认的[模拟合并引用](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request)运行。合并后在实际 `main` 提交上执行基准测试，同时更新可供后续 PR 使用的依赖缓存。多版本兼容性在合并前完成；目标分支变化时先更新 PR 并等待新的检查结果。若启用合并队列，需先接入 `merge_group` 事件。

## 检查范围

| 检查 | 内容 |
| --- | --- |
| Versions and tooling | actionlint 工作流静态检查；CMake、Python、vcpkg 版本一致性；数据导入、CI 规划、交付与发布脚本测试；标签提交归属检查 |
| Linux C++, sanitizers and Python 3.13 | Ubuntu 24.04：无界面 C++ 测试、ASan/UBSan、Python API、九个示例和 C++/Python 对照；标签流程额外构建 SDK 和基准 wheel |
| Linux SDK and installed Python API | 仅标签流程：新 runner 不检出源码，校验交付散列、迁移 SDK、离线安装 wheel，运行消费端、API、示例和 C++/Python 对照 |
| Windows desktop and Python 3.13 | Windows Server 2022 / VS 2022：桌面编译、CTest 与 Qt offscreen、Python API 及示例；标签流程额外构建便携包、SDK 和基准 wheel，验证 SDK 解压迁移及 wheel 安装 |
| Python 3.11 / 3.12 / 3.14 / 3.15 × Windows / Linux | PR、手动及标签流程执行；复用相应平台的原生依赖，编译各解释器扩展，运行 API 测试及九个示例；标签流程改为构建、安装 wheel 后测试 |
| CI passed | 按本次范围检查全部预期作业；应执行的作业失败、取消或意外跳过都会失败，仅允许该范围明确计划的跳过 |

GitHub 的 `main` 分支规则应要求通过 PR 合并、**CI passed** 成功，并要求分支合并前保持最新；禁用绕过和强制推送。仓库中的规范和工作流不会自动修改 GitHub 分支保护设置。

可审阅的服务器规则配置保存在 [main.json](../../.github/rulesets/main.json) 和 [version-tags.json](../../.github/rulesets/version-tags.json)。管理员在 GitHub 应用或更新对应规则集：`main` 必须走 PR，必需的 `CI passed` 限定由 GitHub Actions 提供，合并前要求与目标分支保持最新；`v*` 标签允许新建，但禁止修改和删除。两套规则均不配置绕过名单。规则文件的修改也走 PR，文件本身不会自动调用管理 API。

Linux 和 Windows 的 Python 矩阵分别只等待本平台的原生任务，通过同一份可复用工作流执行，避免跨平台等待和失败连带跳过。`CI passed` 同时检查两个平台矩阵；拆分不改变支持版本和必需测试。

actionlint 在原生构建前运行，固定工具版本与 SHA-256，检查所有工作流及其 shell 脚本。更新版本时同步核验上游发布文件和校验值。

## 缓存与耗时

依赖使用仓库固定的 vcpkg 基线与 overlay，并缓存编译结果。Windows CI 使用专用的 `x64-windows-release` triplet，固定 v143 工具链，只构建实际用于测试和发布的 Release 依赖。本地默认 triplet 不变。首次 Windows 构建需要从源码编译 Qt、VSG、Rocky、GDAL 等依赖，耗时会明显长于后续运行；即使后续依赖失败，已完成的包也会保存到部分缓存，供下次继续使用。缓存区分 runner 镜像和依赖配置，vcpkg 另按编译器及包 ABI 校验是否可复用。这里的 Release 是编译配置，不代表生成或发布安装包。失败诊断保留 14 天，跨作业依赖和标签发布产物保留 7 天。

GitHub 将 PR 生成的缓存限制在该 PR 的合并引用中，其他 PR 和 `main` 不能读取；PR 可以读取目标分支的缓存，详见[缓存访问范围](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)。`main` 的基准测试在验证合并结果的同时维护共享依赖缓存。首次建立缓存、依赖变化或 runner 工具链变化仍可能需要较长编译，不能承诺每次都命中缓存。

排查耗时时先区分依赖安装、项目编译、测试和标签流程的打包。查看缓存是否命中、vcpkg 恢复了多少包、哪些包重新编译；不要用减少 Python 支持版本或跳过行为测试来掩盖依赖冷启动问题。取消日常发布包构建可以减少重复工作，但不会消除测试所需的编译。

Windows、Linux 和 Python 的 `commands.json` 记录每条命令的 UTC 开始时间、耗时与退出码，Actions 作业摘要展示分阶段用时，失败命令同样保留。父任务中的 Python 总耗时包含其嵌套报告，不能跨表重复求和。依赖缓存命中率和安装时间查看对应 Actions 步骤；项目编译缓存需根据这些记录再评估，当前不缓存项目构建目录。

本地日常开发只需验证 Python 3.13；多版本兼容性由 PR CI 负责，不必在开发机安装多套 Python。`pyproject.toml` 的 `[tool.orbitforge.ci]` 统一定义基准版本和测试版本，CI 矩阵与发布附件检查共用这份配置。其他解释器版本只编译 Python 扩展，标签流程再构建对应 wheel。

普通托管 runner 不承担真实 Vulkan GPU 验收；offscreen 测试通过不代表三维呈现或性能达标。带 GPU 的验证仍按[构建说明](build.md)执行。联网 IERS 服务检查是可选测试，CI 不依赖实时数据服务。

## 发布内容

| 文件 | 用途 |
| --- | --- |
| `OrbitForge-X.Y.Z-win-x64.zip` | Windows x64 便携桌面，解压后运行 `OrbitForge.exe` |
| `OrbitForge-X.Y.Z-sdk-win-x64.zip` | Windows x64 / VS 2022 Release C++ 无界面 SDK |
| `OrbitForge-X.Y.Z-sdk-linux-x64.tar.gz` | Ubuntu 24.04 x86_64 Release C++ 无界面 SDK |
| `orbitforge-X.Y.Z-cpNNN-cpNNN-win_amd64.whl` | Windows x64；CPython 3.11～3.15，每个版本一个 wheel |
| `orbitforge-X.Y.Z-cpNNN-cpNNN-linux_x86_64.whl` | Ubuntu 24.04 x86_64；CPython 3.11～3.15，每个版本一个 wheel |
| `SHA256SUMS` | 上述十三个文件的 SHA-256 |

SDK 包含计算库、头文件、CMake 配置、样例数据及链接依赖。将 SDK 根目录和其 `dependencies` 子目录一起加入 `CMAKE_PREFIX_PATH`；Windows 消费程序运行时还需将 `dependencies/bin` 加入进程 `PATH`。具体用法见包内 `README.txt`。

Python wheel 在匹配平台安装：`python -m pip install <下载的.whl文件>`。文件名中的 `cp311`～`cp315` 分别对应 CPython 3.11～3.15，采用普通 GIL 构建；暂不发布 PyPy 或 free-threaded Python 变体。Linux wheel 是本机构建包，不声称 manylinux 兼容；其他发行版或 Python 版本请从源码构建。工作流上传到 GitHub Releases，不发布到 PyPI。

## 发布步骤

以发布 `0.1.1` 为例：

```powershell
python tools/ci/version.py --set 0.1.1
git add CMakeLists.txt pyproject.toml vcpkg.json
git commit -m "chore: prepare v0.1.1"
# 按正常流程提交 PR、等待 CI 并合入 main。
git switch main
git pull --ff-only
python tools/ci/version.py --tag v0.1.1
git tag -a v0.1.1 -m "OrbitForge v0.1.1"
git push origin v0.1.1
```

触发条件是推送 **Git 标签**，提交信息写 `v0.1.1` 不会发布。三个清单的版本必须相同，并与标签精确匹配；`v1.2`、`v01.2.3`、`v1.2.3-rc1` 等格式会在构建前被拒绝。当前流程只处理稳定的三段版本号。

标签必须指向已经进入 `origin/main` 历史的提交，检出的提交必须与标签一致；允许发布 main 中较早的提交，但拒绝未合并的主题分支提交。版本和归属在构建前检查，公开 Release 前再次核验；找不到标签或 main 历史时停止。不要通过重新打标签修复已发布版本，应提交修复 PR 并使用新版本号。

发布作业仅使用本次标签构建产生的文件，核对桌面、两套 SDK、完整 Python 矩阵的 wheel 及其版本后计算校验值，先创建草稿，上传并核对全部附件，最后公开。说明包含平台要求与 GitHub 自动生成的变更记录。上传失败会留下未公开的草稿，可在 Actions 中重新运行失败作业；已经公开的同名版本不会被覆盖，应修复后发布新的版本号。

工作流只需 GitHub 自动提供的 `GITHUB_TOKEN`，不需要额外 PAT。测试作业只有读取权限，只有发布作业具有 `contents: write`；Actions 固定到完整提交 SHA，Dependabot 每月检查更新。
