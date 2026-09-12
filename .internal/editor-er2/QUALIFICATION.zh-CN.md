# ER-2 有限场景编辑候选：最终资格（2026-09-12）

源码候选：`bbe4f65abdedfa91f4075436f5d5a68ae6d893c3`；资产路径修正包含父提交 `ff31bcb1`。
后续报告提交仅补充证据文字，不改变已验证的生产代码或测试程序。
范围为用户批准的 ER-2 有限 Transform3D / Light3D 作者编辑；不进入 ER-3，不合并 main。

## 用户可见结果

- Inspector 将已解析资产显示为挂载路径；实际验证 `/Seed/Meshes/Ground`，空引用和未解析引用使用明确占位文字。
- Transform 的平移、旋转、缩放，以及 Light 的类型、颜色、强度、范围、阴影开关可编辑。
- Inspector 手势和工具栏 Reset selected transform 共用所属 SceneSession 的编辑历史。100 次预览只产生一条历史；Esc、失焦、选择变化、隐藏取消预览。
- Session 独占作者数据、预览和历史；提交只交换预备值，Registry 投影另在 owner 安全点执行。失败保留令牌/operation，可重试。
- 正式 `lux_editor` 默认使用新架构和编辑模式，`--inspect` 为只读；旧 Workbench/Presenter 只作为 BUILD_TESTING 下的 TEST 目标保留，不进入 SDK。
- 当前只支持内存编辑。窗口关闭提示继续编辑或放弃关闭；未提供正式 codec，不承诺持久化保存。

## 与候选绑定的实际结果

| 验证 | 结果与原始记录 |
|---|---|
| 正常 clean clone | tracked snapshot、MSVC RelWithDebInfo all / 第二次 no-op 通过；`qualification/raw`。 |
| 普通 CTest | 137/137 通过；`qualification/raw/ctest.log` 与 `ctest-details.log`。 |
| 普通 GPU | 20 个 SceneGpuTest variant 通过；另有 foreign renderer、Application lifecycle、冷字体应用启动/退出通过。 |
| 编辑画面回放 | 七个捕获状态通过，1014×593；baseline/undo 逐像素恢复，Transform 与 Light 改动产生不同校验值；Vulkan validation errors=0。 |
| 安装与迁移 | 5 个消费者在 sdk 和 relocated-sdk 各配置、编译、无变化构建、运行通过；独立消费者覆盖真实 Scene 编辑。旧 Scene 包/头/DLL 不在 SDK。 |
| 专用诊断 | 同一 tracked 源码，MSVC RelWithDebInfo all/no-op、157/157 CTest、18 个 Scene GPU variant 通过；`current/logs/diagnostic-candidate-*`。 |
| 真实分配失败 | openEditing 15 个实际 Session DLL 分配点；提交 3 个分配点逐点通过，失败后原令牌/operation 重试。投影缺失组件报告 STALE_ENTITY/session/subject，保留作者和历史，恢复后同步通过。 |
| 诊断隔离 | 10 对普通/诊断 DLL 的导入、导出、PDB 模块与分配器来源检查通过；普通 SDK 无故障注入对象/导出；`current/isolation`。 |
| 有限成本 | 普通 clean candidate，1,000/10,000 手势各 5 个独立进程通过；每手势 100 次 preview，完整 execute/project、undo/project、redo/project 和正常 close。独立 checksum、通知数、metadata/retained/staging 限额均有记录。 |

编辑 GPU 校验值：baseline、undo Transform、undo Light 后再 undo Transform 为 `7384170223992026404`；
Transform/redo Transform/undo Light 为 `9302146371259353079`；Light 修改后为 `14073957982339535425`。

每组固定一个作者对象、零 GPU View，独立 100 手势 warmup。1,000 手势 execute/project 为 2.627–2.989 ms；
10,000 手势为 26.072–28.039 ms。每次分别完成 3,000 / 30,000 次通知，checksum 1,500,500 / 150,005,000。
metadata 为 160,080 / 1,600,080 bytes，retained 为 592,000 / 5,920,000 bytes，staging 限额为 65,536 bytes。
这测量的是 headless 作者编辑与实际 Scene 投影，不代表 GPU 耗时、逐操作 p95 或相对旧栈加速。
旧 ER-1 resize/retry 成本样本仍按原身份保留，没有标成新候选测试。

## 限制与失败记录

用户此前确认真实 IME、持续鼠标捕获、中键平移和右键旋转可正常使用；保留该人工结果及原范围。
本轮 Inspector/工具栏输入使用真实 Pane + UiInputEvent 辅助测试；窗口 close-request 的撤销路径已测。
新关闭确认按钮及新增属性输入的完整原生桌面体验仍需人工复核，不能由上述辅助测试代替。
材质/FlowForge 业务迁移和正式 Scene 作者保存仍未实施。

保留开发失败原始日志：首次诊断配置误选 Clang、辅助输入坐标/连续点击问题，以及 Light schema v1
接纳错误改为实际 v2 的修复。不将工具问题计为产品缺陷，也不重标失败日志。
最终诊断的全部程序通过后，报告脚本因隔离 PATH 未找到 Git 而未写出 metadata；改为显式 Git 路径，
单独核对源码和 157/18 份成功结果后补齐 metadata，没有重新标记或代跑测试。

## 交付身份和复核入口

正常与迁移后 SDK 的 lux_editor.exe SHA256：
`FA3614A5B71A2E70F1FB552DC455647ED7286F860FE59F1ABCDE163E93C45F05`。

`source-and-reports.zip` 包含候选 Git blob 源码、本报告、原始日志/画面、安装资格、诊断、成本、manifest。
`source-identities.json` 分开记录 Git blob OID、clean clone Windows checkout SHA256 和诊断工作树 SHA256；
`normal-artifacts.json`、`diagnostic-artifacts.json` 与 `sdk-identities.json` 记录产物身份。未分发字体文件。

本机复核启动脚本：`E:/lux-er2-work/launch-editor.ps1`。它校验固定候选的迁移后 SDK，使用本机字体，
记录正常退出结果，不强制结束进程。可检查编辑、Undo/Redo，以及关闭后 Keep editing / Discard and close。

本轮结论：ER-2 有限场景编辑候选及自动化/安装资格已交付，等待独立审阅和新增 UI 的人工体验反馈。
