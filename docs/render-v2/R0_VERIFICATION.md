# Render V2 R0：独立验收

**R0 PASS — Legacy Freeze / Isolated Build Bootstrap。**
日期：2026-10-10。仅冻结、输入导入、隔离入口与证据保护通过，不代表 Render V2 行为或 V1 全项资格通过。

## 提交与执行身份

- V1 frozen/base SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- R0 implementation SHA：`7a448d595b0124aa33b2bc8ec52ff0183f566538`。
- 分支：`codex/render-v2`。本报告独立提交，verification SHA 用
  `git log -1 --format=%H -- docs/render-v2/R0_VERIFICATION.md` 解析，不自引用未生成的 commit hash。
- 施工 worktree：`C:/Users/ChenHui/.codex/worktrees/render-v2/lux-engine`。
- 资格源码：`D:/LuxQualification/render-v2-r0-7a448d595b01/source`，从 implementation commit
  `git clone --no-hardlinks --no-checkout` 后 detached checkout；不是原用户工作区或施工 worktree。
- 资格 build：`D:/LuxQualification/render-v2-r0-7a448d595b01/build`。
- V1 oracle：`C:/Users/ChenHui/.codex/worktrees/render-v1-oracle/lux-engine`，clean detached frozen SHA。

先在 clean implementation 上执行 ValidateTrackedSnapshot，再创建独立 clone；clone 和 oracle 也分别检查。
构建与验证串行，没有 V1 product build、GPU、SDK 或 Android configure/build/test。
工具：CMake 4.1.2、Ninja 1.11.1、MSVC compiler 19.44.35228.0（工具目录 14.44.35207）、
Python 3.13。bootstrap 使用 C++20、RelWithDebInfo；清除旧 build/install PATH 及相关前缀环境变量。

## 实际结果

| 检查 | 结果与范围 |
|---|---|
| 附件 | 12/12 SHA-256 一致；独立工作单与 ZIP 对应文件相同；Git 中正文原字节保留 |
| Frozen source | 719/719 原路径精确迁移；mode/blob/size 一致，38,439,280 Git blob 字节 |
| 旧目录 | 六个原闭包均从活动路径迁出，没有旧位置替身 |
| 修改范围 | 只包含文档、bootstrap、archive guard 和原样迁移；Root/Editor/其他消费者实现未修改 |
| 历史文档 | 47/47 原文/原 blob 保留；实际删除集合 `[]` |
| 历史证据完整性 | MA11 5818、UBSan 138、分类 119、Actions 94，共 6169 文件哈希匹配；只核验原证据 |
| 用户修改保护 | 原分支/HEAD、6 文件原始 SHA-256、staged/unstaged binary diff、status 完全一致；未跟踪集合仍为空 |
| Bootstrap configure | exit 0，生成独立 Ninja 工程 |
| Bootstrap all | 两轮 `--target all -j 4 -- -k 0` 均 exit 0；第二轮 `ninja: no work to do` |
| Legacy guard | 直接配置 archive 返回预期 exit 1，命中 frozen reference 诊断 |
| 静态隔离 | 扫描 1093 个 tracked 活动源码/CMake 输入，没有 render_legacy 引用；不以 archive 内部代码作为活动输入 |
| 真实构建图 | File API：0 生产 target、28 CTest utility targets、104 个 CMake inputs；没有 legacy source/include/link/codegen 输入 |
| 安装 | bootstrap 无 install target/生产安装规则；没有 install manifest；未运行安装或修改三个开发 SDK 前缀 |
| 生产 C++ | 新 Render V2 C/C++ 头/源为 0 |
| 测试库存 | `ctest --show-only=json-v1` 返回 0 测试；没有将空 CTest/空构建写成 Render 行为测试通过 |
| 验收 clone | 完成后仍 clean |

普通 `git diff --check` 会报告附件 00/11 原文的 9 处 Markdown 双空格换行。
按字节保留输入要求没有清除这些原文格式；新增 bootstrap/检查脚本/清单说明/已知问题/通信审计
的定向 whitespace 检查通过。冻结源没有任何 whitespace 修改。

## 证据与复核

证据根：
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R0/7a448d595b0124aa33b2bc8ec52ff0183f566538/`。

证据 manifest SHA-256：
`c10604efd3e26e43cda485c5cc6de497578194020dc3556724f82e37a5ea8eed`。

15 条独立资格命令的 argv、cwd、UTC 时间、退出码、预期退出码、log/hash 在 `commands.json`。
原工具链、CMakeCache、build.ninja、File API、空 CTest 库存与保护快照都保存在同一 manifest 中。
`preflight/` 保留完整原用户差异、输入生成/历史证据核验脚本及该轮实际 qualification runner。
manifest 对全部证据使用相对路径，未覆盖或改写此前 EditorFramework-v2 归档。

关键证据：

- `static-results.json`：冻结源、范围、历史文档和活动构建输入检查。
- `bootstrap-configure.log`、`bootstrap-all.log`、`bootstrap-no-work.log`：入口与两轮构建。
- `legacy-guard.log`：有意拒绝 archive 配置，不是未解释的构建失败。
- `file-api/`：真实 codemodel/cmakeFiles/toolchains 回复。
- `user-protection.json`：原工作区不变证明。
- `preflight/historical-integrity.json`：四个历史归档的完整性及原实现身份。

源码清单与算法恢复入口见 [LEGACY_MANIFEST.md](LEGACY_MANIFEST.md)；源清单 SHA-256：
`b4a4cb0b92479ba1b91e99c76aa0f26a1fd01820577a2b0855d2b435613fe91c`。
历史限制见 [V1_KNOWN_ISSUES.md](V1_KNOWN_ISSUES.md)，调用点与未来向量见
[V1_TRANSPORT_AUDIT.md](V1_TRANSPORT_AUDIT.md)。

## 明确保留的未运行范围与交接

- V1 TOOLCHAIN / EDITOR / PLAYER / MSVC-ASan 在 frozen SHA 上本轮全部 `NOT_RUN`。
  既有结果逐条绑定原始 implementation/receipt SHA，不能作为本次全矩阵通过。
- V1 GPU/SDK/性能未重新执行；UBSan 100/130 等历史判定未被覆盖或修复。
- V2 EDITOR / PLAYER / TOOLCHAIN 为 `EXPECTED_UNAVAILABLE`，直到 R17 正式 cutover。
- V2 Render 行为、GPU、性能与 installed SDK 为 `NOT_RUN_NO_R0_PRODUCTION_TARGET`。
- 旧源码没有安装到新构建，也没有同步任何改写后的公共头；未来 R1 新公开头另行履行三前缀规则。
- 未创建或切换旧/新 renderer 开关，未添加 compatibility target/adapter；没有推送或合并。

交接：R0 PASS；V1 frozen `a669409a289a6fa4092f21176397795b1cdb7f3e`；V2 HEAD 为包含本报告的
独立 verification commit；legacy 清单与 known issues 如上；bootstrap PASS；
V2_PRODUCT = EXPECTED_UNAVAILABLE；R1 allowed target = render_core only。**STOP，不进入 R1。**
