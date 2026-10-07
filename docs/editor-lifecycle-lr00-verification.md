# Lifecycle LR00：固定基线审计

LR00 审计门禁 **PASS**。没有修改生产行为，也不代表 LR01–LR08 已通过。
终态前置实现为 `fb468d062f2e6fb39c2f8e90a55f12bb19e982db`，验收及本次审计基线为
`b2b9a6bac2f9f3e5fc70bae32271c30a7f6f85d4`；lux-cxx 为
`0a0e7419fc7229df6e372cd35a540249f92250ef`。

施工规范：《LuxEngine 全仓生命周期、无异常构造与显式状态整治》，SHA256
`b21f4c4676c4e0eab8a75cb9e560c96065e0340a3925591c9f233f56d8b41712`。
唯一可变材料仍为 `.internal/editor-redesign/terminal-architecture/followups/lifecycle/`。

## 审计范围与证据

实际 Git blob 清单包含 1,255 个生产 C++/模板文件，12,193 个逐命中记录，其中代码命中 10,905，
注释 1,176，字符串 112。冻结参考源码 533、测试 77、资格输入 14、第三方 9 个文件分别登记。
生成器的输入、输出职责另有人工核对；宏/模板解析不完整的 12 个命中没有被跳过。

每个命中记录路径、类型/函数、符号、分类、原语义、目标语义、阶段、是否修改、理由、测试责任，
以及原 blob、行列、源码与分类规则。`lifecycle-audit.csv` 与 `lifecycle-audit.md` 从同一数据生成。
未分类项为零；750 个命中需要整改，合并为 407 个文件/类型或函数责任行。
重复声明和调用计入命中数，不能把它当作独立问题或运行测试数量。

| 分类 | 命中 |
|---|---:|
| C1 构造 | 483 |
| C2 部分所有权 | 150 |
| C3 登记 | 40 |
| C4 语义结束 | 35 |
| C5 物理退休 | 382 |
| C6 可选载荷 | 12 |
| C7 构造/运行分离 | 20 |
| C8 真实协议 | 3,776 |
| C9 可空 owner | 386 |
| C10 词法或类型误报 | 6,909 |

验证器复核了全部生产 blob 哈希、清单与逐命中覆盖；实际删行、改 blob、未知分类均被拒绝。
这里只验证审计材料完整性，没有制造新的 CTest、SDK 或 GPU 成绩。

## 冻结整改范围

- LR01：Window/GLFW/Root，及实际发现的 TrayIcon、Physics2D、CppStatic/Lua 构造状态。
- LR02/LR03：先补缺失的 Vulkan/VMA 叶子与批量所有权，再迁移完整资源家族及所有真实调用。
  包含文档示例之外的 Canvas2D、EVSM、Light、Material、Instance、Terrain、RenderCluster、稀疏表及流。
- LR04：VFS mount、Input activation/priority、Vertex source 登记；不为不可变目录增加 lease。
- LR05：VFS endpoint，以及真实 ScriptRealDelayProvider/ScriptAssetAccess 的阻塞析构。
- LR06：Present、Scene active/retired、RenderRuntime/transfer 的实际 GPU/线程排空职责。
- LR07：Hook、timer、swapchain、RenderRequest、Flow LastLink、ShadowMap 重复构造标记，
  完成 serial、navigation predecessor、ScriptAsset 结果和编译图完整发布。

完整 owner、文件、符号、目标和验证责任见归档 `must-change.md`。保留既有 VmaBuffer/VmaImage、
RenderSurface 等正确 owner；非拥有 GAPI carrier 的释放原语不等同于需要删除的所有权协议。
Script gameplay、Process/Object 同步、UploadLifecycle、Feature 事务和帧内 prepare/publish 均有保留依据。
不能仅凭 `bool`、`prepared` 或 `shutdown` 名称进行替换。

## 归档与保留范围

原始输出在源码树外：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr00/frozen/`。
20 个归档文件；manifest SHA256：
`d93fcda342edbca8f0681182e34e3b64019c81aacf3f8d1d8ffe66886b901557`。
中文/空格路径搬迁通过；删除、篡改实际 CSV 后验证均拒绝，恢复原字节后通过。

Context/Pane 用户排版差异仍未提交；ProjectBuilder 补丁独立保存、未应用。main 与历史快照不变。
`Q-P06-CLANG-GET_DELETER` 和 UBSan `NOT_QUALIFIED` 保留，不能以恢复 RTTI 或削弱代码 pin 绕过。
LR08 的实际 Linux、sanitizer 资格仍须执行；旧输入/IME/性能延期不改判。
按用户连续实施授权进入 LR01，不等待新的阶段批准。
