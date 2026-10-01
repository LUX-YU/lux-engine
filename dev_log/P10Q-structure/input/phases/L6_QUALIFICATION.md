# L6：同一最终 SHA 的资格、删除检查与交付

本批验证前五层已经落实，不再借验收临时重设计所有模块。任何修复继续追加实现提交，最终只绑定一个新的 implementation_sha。

## 1. 最终状态冻结顺序

1. 检查全部 L0–L5 去向、target、抽象和旧消费者处置已闭合。
2. 导出最终 tracked diff，逐文件确认没有用户原修改被带入。
3. 提交最终实现，记录完整 SHA；在独立干净检出和新构建/安装前缀建立资格。
4. 显式 P10Q，启用结构严格模式，不能停在 L0 全旧路径豁免模式。
5. 运行构建/回归/安装/实际模式；发生代码修复就形成新 SHA，并重跑受影响与最终资格，不能用旧构建成绩覆盖。
6. 实现稳定后冻结 dev_log/P10Q-structure，独立证据提交；不把证据自引用 SHA 写入自身哈希循环。

## 2. Windows 工程验证

- 复用 P10Q 已验证的工具链和精确第三方版本，生产保持 C++20、禁不必要扩展。
- 从实际收据取得完整配置和依赖参数，不猜配置变量或仅用手写一个 cmake 命令替代 SDK 闭包。
- 新 build tree 首次全量构建，随后第二轮无工作；首次失败与修复日志保持。
- CPU-only 模型/interaction/TaskMonitor/纯活动 consumer 不要求启动/链接 UI；实际 backend closure 分开核验。
- PLAYER 保持无 Editor 编译/安装输入；不能仅以没有创建编辑器窗口作证。
- 所有新的 public header 独立 include；C++20 负例含原八项 operation 特殊成员拒绝。
- 原第二编译器可用时保留，其不可用如实标记，不把 clang-cl 当 Linux。

## 3. 功能与真实桌面

从实际 baseline 测试清单构建行为映射。原报告数字 204/204、CPU178/178、PLAYER11/11 仅是索引，不要求新版本保持相同总数，更不允许减少行为。

最终至少涵盖：三作者/所有历次 R1-R2、保存/真实IO、Run/晚读结果、workspace/selected迁移、交互BUSY与来源、Host生命周期、目录/任务、compile/固定对象retry、GUI graph/input。

旧 GPU_UI、EDITOR_SCENE_PANE 和新 SceneView 双视口须按真实配置和命令区分；更新文件位置不能使模式回落默认 CPU_UI。新双视口需要仍有真实输出/隔离/拾取/退休观察和 validation 记录，不以空白输出或纯 key 表替代。

Windows 原生输入继续按当前可用设备和既有脚本运行。系统 IME 候选/组合/提交若无实测仍 NOT_RUN；Unicode 注入不是 IME。

## 4. 安装消费者

- 全新 install prefix，不读取旧 build tree 的头/DLL。
- 原全部适用 SDK consumer 使用真实公开入口；测试源若移动，变更 source path 而不借内部实现头。
- 新增/调整 TaskMonitor CPU consumer；新 E0/E1 纯值/模型独立 consumer；通用保存策略不链接具体模型/文件后端的 consumer。
- 静态 dependencies、PIC、导出宏和旧 DLL 搜索路径由实际 link/loader 结果验证。
- 安装清单不能含旧转发头、旧已删除 alias 包、测试访问、内部 sinclude。
- 若只移动物理源而公开逻辑 include/target 没变，这不是“没整改”；验证它指向唯一真实文件与正确层。

## 5. 依赖与负例

执行 `04_BUILD_AND_DEPENDENCY.md` 的真实正反夹具。每一负例均需：基准正例成立 → 加非法边命中目标规则 → 去除同一边正例恢复。未知 imported leaf、LINK_ONLY、生成 include、模板实例化不可以漏掉。

不要让 consumer-only 阴性测试因为缺少 Vulkan/LLVM/SDK 目录就“通过”。

## 6. 性能与跨平台范围

- 不补旧 50k 深链到100样本。
- 保留短的真实复杂度/原子性/容量/SharedBytes owner/画布整理回归；纯目录迁移不重新跑 BQ1–BQ5 全计时。
- 算法或数据结构若在本轮变化，只测对应路径、记录规模/机器/分配范围，不能用比例推算成绩。
- Linux 当前没有环境：源码/平台分支/路径/CMake规则审查写明，构建运行为 NOT_RUN；不阻塞按新用户范围验收。
- ASan 与全量 SDK 插桩不一致造成的未资格仍如实保留，不关闭检查掩盖链接失败。

## 7. 删除检查

按 file_plan 和 symbols 作肯定式核对，而不是只 grep 名字：
- 原源码不再参与 compile，旧物理文件已删除或明确暂留。
- 原逻辑定义恰有一份；无同名 ODR 副本/重复生成注册。
- 被删除的层级/包没用 alias 或 include 壳恢复。
- 新目录没有原 Context/PaneManager 旧业务的改名副本。
- 保留的旧产品文件有真实消费者、没有新增消费者且 P11/P12 期限明确。
- 不删除历史收据中的旧名称，grep 报告区分 active source 与 dev_log。

## 8. 证据搬运与哈希

归档检查继续只读相对路径和固定 Git 对象。复制归档到含空格/中文路径后校验；缺必需真实日志、篡改日志必须失败；恢复原 bytes 后成功。

这证明归档自洽与可迁移，不证明未测平台通过。不要要求历史 verifier 针对新路径验证旧源码；每份收据始终用自身 implementation_sha。

## 9. 出口判定

以下全部满足才标记 `P10Q-structure: PASS`：
- 五层长期代码去向闭合，没有无法解释的逆向生产依赖；
- 修改中的 owner/invariants 原行为回归成立；
- 真实 Windows/SDK/相关 GPU 与输入通过；
- 文件、符号、target、安装、文档、模板实例化图一致；
- Linux/IME/历史性能范围单独列明，不冒称完整跨平台；
- 当前原产品与新链状态明确，P11/P12 尚未实现内容没被伪称完成。

最终摘要采用 `07_RECEIPT_AND_RESUME.md` 格式。正常推送既有实施分支后停下等待复审，不自动实施 P11。
