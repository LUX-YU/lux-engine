# P03 R1：重载输入所有权补正

状态：**PASS（P03 R1 范围）**。从 `7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb` 继续，
补正实现为 `90d74819a358ac722c002fd544b88cb4fbb454a1`。分支 `codex/editor-redesign-v4`。
只完成本轮 Material 私有重载输入补正，不进入 P04，不修改 main。
原 `dev_log/P03/` 保持历史快照；唯一可变施工材料仍为 `.internal/editor-redesign/`。

## 唯一输入 owner 与清理顺序

`PreparedMaterialReload::prepare()` 内建立局部 `ReloadInput`，按 **code、source** 顺序声明成员。
函数入口即消费两个参数，节点的拥有关系不再依赖参数的构造/析构顺序。此类型不公开、不安装、不用于移动赋值。

- 未能取得 READING：外层输入单元销毁 source 节点，再释放 code。原 CLOSING/READING 保持不变，没有自行解除别人的 gate。
- 取得 READING：lambda 的第一条语句将输入移动到自身局部对象；此后才校验 binding。
  所有提前返回均先在 READING 内销毁输入节点、再释放 code，最后退出 ReadScope。
- 正常进入 create：移动 source 并保留局部 code；继续复用既有 `InputRelease`、候选校验和 loaded 基线。
  不修改 create、adopt 的既有身份/History 判断，不删除未绑定拒绝。

未增加另一套 busy、gate、公共输入框架或会话 owner。History、SessionStore、SessionState、快照和顺序批次生产实现均未改动。

## 真实负例与修复前后

修复前以固定验收源码加新增测试构建；`before/production-before.hpp` 与前置 Git blob 一致。
`before/regression.patch` 只包含测试及现有 target 的测试登记，与最终实现中的测试差异一致。
测试使用真实 MaterialSession/MaterialSource，通过公开 apply 从输入节点析构回调发起 rename；不是简化控制流探针。

| 场景 | 修复前实际结果 | 修复后 |
| --- | --- | --- |
| R03-01 未绑定，保留外部 code 强引用 | **FAIL**：prepare 拒绝但 `nested_edit=1`；完整源、历史、observed 改变 | INVALID_SOURCE；析构回调处于 READING，编辑被 BUSY 拒绝；全部状态不变，随后正常编辑成功 |
| R03-02 未绑定，仅输入最后 lease | **FAIL**：同样发生重入编辑；本机未观察到代码提前释放 | 两个输入节点都在代码有效时析构；code 最后释放，无泄漏；失败不改变会话 |
| R03-03 外层 CLOSING 与外层 READING | 两种分支在本机均通过；不能证明原参数次序可移植 | BUSY；既有 gate 原样保留，最后 lease 在节点之后释放；外层结束后正常编辑成功 |
| R03-04 错误 AssetId、clone 异常、正常重载 | 既有路径通过 | 清理覆盖回调、异常和最后 lease；成功采用保持 SessionId/绑定、更新 HistoryId、恢复 loaded checkpoint；过期候选拒绝，旧快照不变 |

原始输出分别见 `before/reload-*.log` 和 `logs/material-reload-*.log`。本机为 Windows/MSVC；
最后 lease 的修复前销毁次序未在本机失败，如实保留该结果，不冒称全编译器复现。
复审包自带的 Clang/GCC 最小探针仅作为提供方的历史证据归档，本轮未把它当作真实引擎验收成绩。

完整不变性检查包括：源编码、History current/cursor/entry_count/revision/event_sequence/charge/closed、
observed、dirty、SourceBinding、BindingRevision、persisted checkpoint 和准入恢复。
原测试的 Saved 校验只增强，没有减少原断言。独立计数验证每个动态输入节点确实销毁，最后 code 释放时没有存活节点。

## 工程验收

- 对实现 SHA 显式配置 `LUX_EDITOR_MIGRATION_STAGE=P03`，全量 `target all -j 4 -- -k 0`，第二轮无工作。
- 完整 CTest **64/64**：原 **59** 项及断言保留，在同一测试 target 新增五场景；不以数量替代上述行为结果。
- 原 X03-01～04、混合批次、稳定读取、P01/P02/R1 与受影响旧产品回归全部重跑；对应领域 Q 范围沿用 P03，未提前验收后续服务。
- SDK 重装，七组安装消费者重新配置/构建/二次无工作/运行，合计 **8/8**。
  没有公共头变化，不需要再次同步三个公共头前缀；不执行 Android 构建。
- 实际 CMake 依赖负例、修复后图、P03 迁移检查与包审计通过。tracked snapshot 门禁后从实现提交的独立 clean clone 验证配置和实际闭包。
- 原 P03 收据验证器通过；P00/P01/P01-R1/P02/P02-R1 继续按各自实现 SHA 验证，不改历史记录。
- 最终执行 `python dev_log/P03-R1/check_receipt.py`。归档迁移及故意缺失/篡改证据检查见 `validation/`；缺失不跳过。

## 保留失败与范围

C01 仍 **FAIL**（P09/P12），C03 仍 **FAIL**（P11），C04 仍 **FAIL**（P12），原复现和后续责任不变。
本次新 Material 问题已在本轮补正，没有挂到旧 C03 或延期到 P05/P11。

生产改动仅在 `model/src/PreparedMaterialReload.hpp`；另改 `model/test/material_session.cpp` 与
`model/CMakeLists.txt` 登记测试。没有移动目录、更改 target/include/package 名称或增加库。
旧 UI/预览/编译/IO 适配及唯一私有桥保持 P03 已登记范围和 **P12** 删除期限。
不引入新的 DLL 加载矩阵或人工桌面/IME 验证；原自动产品/GPU消费者已重跑。

文件清单在 `files.json`，命令、结果及哈希在 `receipt.json`、`artifacts.json`。验收记录独立提交于实现之后。
停在 **P03** 等待复审。
