# P05 验收快照

前置：`aad13c594afe33af69fcfba1b44790b4e9ad148d`（P04 R1）。
实现：`8deaee9beaab431c1cfa82282206e7ff2f40d09b`，分支 `codex/editor-redesign-v4`。
最终状态由本目录 `check_receipt.py` 对 `receipt.json` 和归档证据核验；本轮停在 P05。

## 交付与所有权

三个实际作者模型均实现冻结保存、Save As、Export Copy、具体 codec 与解码值→owner 构造。
SessionStore 仍唯一拥有会话；SessionState 仍唯一拥有基线、绑定、观察版本和准入；History 算法不变。
SaveService 拥有保存记录，借用可撤销的角色；唯一 WriteCoordinator 拥有目标 FIFO 与未结发布责任。
普通保存释放读取准入后可继续编辑。Save As 使用已有 BindingChangePermit 与私有 PreparedRebind，
成功不清历史、不重建图，失败不改原绑定，Unknown 释放编辑许可但不释放目标 lane。

新生产者均借用同一个协调器。未来编译、Workspace、Open/Close UI 仍在其原阶段，没有提前实现。
旧产品没有在 P05 切换新链，不能将此验收扩大为新旧 writer 并写同一目标的保证。

## 验收证据

- 显式 P05；全量 all `-j 4 -- -k 0`、二次无工作、完整 **104/104 CTest**。
- 原 **88** 项测试名保留；门禁核对原测试源码和断言，没有缩减。
- X05-01～09 与 Q11～18 的 P05 范围：三模型的真实文件、反序编码、Undo 新保存意图、反序回执、
  真实路径/权限失败、发布后 durability 失败、未知写者隔离、关闭后槽位复用、Save As 身份高水位。
- 17 组真实 CMake 依赖用例；每个非法边/头都先拒绝，再移除违规后配置成功。
- SDK 重装；原八组九项安装测试与新增持久化消费者重新构建执行。新消费者只用安装 SDK，
  不链接旧 Editor/UI/SceneRuntime，真实创建、保存、失败、重绑定并检查文件与基线。
- 独立 clean clone 核对配置与实际闭包；历史收据按各自 implementation_sha 核验。
- 门禁另以只有归档文件与 Git 对象的搬迁目录执行；缺失或篡改必需证据均拒绝。

详细命令和 SHA256 位于 `receipt.json`、`artifacts.json`；新增/修改文件逐项列于 `files.json`。
开发期失败日志也归档，不能用最终成功结果代替或删去。

Q50 本轮实际测量 72 次保存：约 340 ms，编码输出合计 327414 字节，普通分配计数 15748，
进程工作集峰值 11485184 字节，保留票据峰值 3，ack 后 0。数值仅是该次 Windows 测量；
分配计数不包含第三方 DLL/aligned allocator，不代表性能提升或全产品性能验收。

## 迁出、删除与暂留

ProjectPublication 原文件 IO 算法体迁为 `editor_file_publication` 的唯一实现，旧路径仅保留错误映射。
旧 copySceneSource 的包复制算法体删除，共用 engine 的 copyScenePackage。未通过删测试或删产品功能过关。

TAssetSave、SceneSave/MaterialSave/FlowSave、SceneSaveCapture::copied 及旧工具保存 API 仅供旧产品
和旧回归消费者暂用，最迟 **P12** 删除；具体名单在 migration-ledger.json。新路径无其反向依赖。

## 保留的限制与旧失败

C01、C03、C04 仍为原 **FAIL**，日志、责任与期限保留，未借本轮修复或改判。
没有新增未通过的 P05 必测项时，阶段门禁才允许 PASS。

本后端是单目标命名替换与乐观版本检查，不承诺跨进程 CAS、多文件事务或掉电后的目录持久性。
已知硬链接别名拒绝；并发修改符号链接/外部写者不在本协调器保证内。Windows 使用大小写无关键。
索引场景的内容重建仍需其索引 builder；未知根关联的 Save As 在写入前准确拒绝，普通保存保留未知载荷。
本轮未修改 modules 公共头，未执行 Android 构建；原 SDK 消费者的桌面/GPU 回归照常运行。
