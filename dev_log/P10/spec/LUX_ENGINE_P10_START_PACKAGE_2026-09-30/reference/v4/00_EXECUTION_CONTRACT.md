# 所有阶段共用的执行契约

本文件与对应 Pxx 文件共同构成实施边界。阶段说明不是建议性清单：新增类型、所有权转移、调用方迁移、旧符号删除、构建与测试是同一个交付，不能只选其中容易完成的部分。

## 1. 给执行 LLM 的起始指令

将以下段落与本阶段 Markdown 一起交给实施者；从第二阶段开始还要提供前阶段实际代码与收据。不要只贴聊天摘要。

> 你只实施本次给定的 Pxx 阶段。先读取仓库中 docs/editor-redesign 的基线、architecture、migration-ledger、test-coverage 和前阶段 receipts；核验实际 HEAD 与前阶段 implementation_sha 的祖先关系。按阶段类型表实现真实行为，按删除账本迁移全部调用方。不得反向依赖旧 Context/Editor Impl，不得增加永久兼容层或第二产品入口，不得删除功能/测试来换通过。用真实命令与日志证明完成；任何必须门槛未满足则输出 PARTIAL/BLOCKED，保留失败，停止，不进入下一阶段。最后提交本阶段实现和证据，准确报告已删除项、到期未删项与测试情况。

## 2. 阅读与权威顺序

本包总契约＋所给阶段的具体要求优先于 V3 的示意实现顺序；V3 的领域语义/不变量继续有效。模块依赖见附录 D，细粒度旧代码去向见附录 B。对本包没有覆盖的新提交事实先记录差异，不能强行按过期行号删代码。发现真正矛盾时给出受影响的类型/流程/测试与最小修正决定，在 receipt 中记录；不得以“优雅优化”为由自行扩大架构范围。

本包不声称包含全仓库符号穷尽表。P00 必须补实际 AST/引用/生成/安装消费者。每个关键 Impl 的所有非空成员、每个方法、每条外部消费者都必须归类为 MOVE、REPLACE、DELETE、KEEP_WITH_REASON、TEMPORARY_UNTIL，未知项阻塞对应阶段，不能静默遗留。

## 3. 版本和用户工作区保护

固定调查基准是 2bb33ff1a1f11025cf404e074c0e9b259239d8a4；之后各阶段在前阶段已验收实现的后继提交实施，不每次 reset 回调查基准。记录 git status、HEAD、前阶段提交关系、工具链/依赖版本与工作区差异。保留用户未提交修改、真实日志、资产和布局备份。不得 reset --hard、git clean -fd、删除用户 branch 或覆盖整个 repo。允许创建单一实施分支；不自行合并 main 或发布 release。

## 4. 类型/函数/成员的完成条件

新类型必须有一个清楚的事实或资源责任。每个 public 方法记录前置条件、立即返回、后续完成、失败后状态、owner thread 和借用有效期；不是全部套 expected/noexcept。每个拥有资源成员注明最后使用点，尤其回调、memento、worker、GPU submission 和插件代码。

组合优先用于具有不同生命周期的责任；继承只用于真实可替换边界：异构 IEditSession、保存/编码副作用接口、真正的 Pane/Element。禁止把 Scene/Material/Flow 继承自一个含 save/play/compile/UI 的万能 Document。不要在每层增加 IManager、ServiceLocator、get<T>、any、void* 服务字典；底层已有合理 type erasure 并非全仓库禁用。

编辑器 owner-thread 流程复用原执行器；新域不自建线程池。同步小操作直接函数调用，异步票据只用于跨时刻操作。只有无分配/no-fail 的结构 commit 才可承诺不失败；未处理分配失败的代码不能一律 noexcept 后 terminate，却在报告中声称恢复成功。

## 5. 原代码何时可以暂留

P02–P11 的新域模块在非安装产品的行为测试切片中运行；允许旧产品壳继续存在到 P12，但不允许同一个用户工作副本被两个架构同时操作。限期兼容属于迁移成本，不是最终设计。禁止永久 LUX_USE_NEW_EDITOR selector、NewEditor/EditorV2、以旧 Context 转发的“新”访问器。

P01 必须在原产品中也只使用纯化后唯一 EditHistory，因此允许一份 LegacyPersistenceState。它不保存第二份作者真相，不依赖新业务反向壳，只在 editor/transition，由旧产品消费，P12 删除。P08 原 rooted UI 构造也只供旧产品，P12 删除。其他临时条目必须逐项登记：精确文件、调用方、无双 owner 证明、最晚阶段，不可用“compat 目录整体例外”。

旧数据兼容不同于旧架构兼容：LegacyWorkspaceImporter 可保留，只解码旧载荷到新纯值，不能 include/instantiate 旧协议和 owner。不得以删除垃圾为名删用户备份或丢失未知插件状态。

## 6. 每项删除须闭合的八个位置

声明与定义；函数调用和成员访问；friend 与 test access；模板/反射生成输入；CMake sources/targets/aliases；安装头/包/导出和 SDK 输入；示例/插件消费者；活动测试与文档。只从 target_sources 移除但留源码、改扩展名、注释整段、backup/unused/deprecated、EXCLUDE_FROM_ALL 都不算删除。

旧 public C++ API 可破坏并在同阶段迁消费者，不承诺旧编辑器 SDK 二进制兼容。用户资产/布局兼容需要明确读取适配；旧 runtime 插件独立边界不能因 editor-only 变化无故失效。

删除账本不是“所有名字零命中”：名字可以出现在证据和旧格式 fixture 中。必须禁止的是可执行旧 owner/公开别名/旧调用链。相反，旧名字消失但新类保留同一个巨大 Impl& 或 SharedEditorState，仍判失败。

## 7. 模块、文件和代码生成规则

使用本包目标目录和逻辑 target；实际 CMake 名称从 existing helper 正确映射。公开头只导出必要类型，私有实现留目标自己的 src/private 头。禁止 `${PROJECT_SOURCE_DIR}/editor/*/pinclude` 跨模块取头，不准以相对 ../../../ 路径绕过。

生成的元数据、Inspector codegen 和 SDK signature inputs 随类型归属迁移，不能留指向被删除 SceneEditor.hpp 的 TARGET_FILES。生成产物的正确重建比手动改生成文件更重要。测试注入放独立支持 target，不把 for-test-only 写入口安装到产品 SDK。

类型表中的 `{A,B}` 表示两处具体文件，阶段开始应展开在 implementation plan；以 `/` 结尾表示私有实现目录下的小类型，必须给出实际文件。不能把这些占位写成字面文件名。一个类型不一定单独一个文件，但共文件必须具有同一责任。

## 8. 测试与资格

每阶段至少执行本阶段 X 场景、修改影响的原测试，以及测试矩阵规定该阶段达到完整门槛的 Q。相关 Q 的 lower-level 子测不冒充完整桌面/GPU；未来阶段尚未存在的功能不建空 stub 凑全覆盖。所有适用性在 P00 记录，之后更改必须有实际原因而不是逃避失败。

native/desktop/GPU/toolchain/installed/sanitizer/benchmark 分组明确。Native 不强依赖 PowerShell 或本机 LLVM 绝对路径；要求真实后端的阶段缺环境只能 BLOCKED/PARTIAL。不得用 mock 渲染器代替双 View Vulkan validation，也不把日志中的无错误输出当作测试实际执行证据。

每条测试记录 input、命令、环境、exit code、log hash、实际断言结果。禁止删除失败测试、放宽容差、改成默认 SKIP、把异常吞掉、强制 clean=true、让危险路径固定返回 success。负例必须证明失败来自预期规则，而非无关配置错误。

## 9. 收据、分段提交和中断

收据模板在 manifests/receipt.template.json；它是未完成模板，不是 PASS 证据。测试过的实现 commit 先创建，receipt 后写到单独证据提交并指向 implementation_sha；不要求文件写入自身最终 commit SHA。日志可在外部证据包，仓库收据记录路径和 SHA-256，不能把无链接的“本机通过”当可交接证据。

任务在中途遇到真实失败，允许保留可构建的中间提交与 PARTIAL；不能在失败后仍标记本阶段门槛通过，也不能自动开始下一阶段。下位实施者先完成前阶段缺口。每阶段末输出新增 owner、迁移/删除 ID、必测结果、仍有效的限期项和明确 PASS/BLOCKED/PARTIAL。

## 10. 审计工具的地位

scripts/audit_migration.py 是只读的文本/路径种子检查，不会删除、移动或提交文件。它补捉过期旧文件、旧类型定义/别名和未登记过渡文件；不理解完整 C++ AST，不计算真实编译依赖，也不证明新类型语义正确。仍必须由 P00 建真实 compiler/AST/CMake guard 并在各阶段更新。

工具出现误报时添加窄解释与更准确规则，不能整目录忽略所有代码。固定本包 seed 不足以证明全库清零，必须使用 P00 完成的实际账本交叉检查。所有自动发现问题均保存证据，不提供自动删除选项。
