# P05 R1 验收记录

前置验收：`5866f990f8a5c4d68e19e0e395c9487c6c0f2779`。
补正实现：`1b2366616ba918bba29a3dcd1349274c574de398`，`codex/editor-redesign-v4`。
本记录只覆盖 P05 R1；停在 P05，未进入 P06。最终状态以本目录 `check_receipt.py` 为准。

## 两项补正

**B01：SaveService 的可调用性与记录执行期。** 一个私有 RAII DispatchScope 覆盖 owner 调用、
角色回调及回调输入清理。期间 status 与注册 token 析构有效；递归修改返回 BUSY，递归采用延后。
describe/capture 返回后重新检查原注册；撤销则拒绝并回收票据与捕获额度，不改投新角色。
accept/rebind 及其清理过程不能递归采用或 acknowledge 删除本操作。已经进入的 accept 可以完成，
撤销阻断未来调用，不抹去已有 Published。没有新增 Session busy、公共 API 或通用管理器。

**B02：确认观察记录与版本衔接分离。** 原记录保留的有效发布前提与成功输出组成一条已验证版本边。
只在同规范目标、有效 Session/Binding、当前 chain_begin 与前序 ticket 范围内使用成功边。
因此 W1 删除后，W2 已成功消耗 V1 的事实仍足以让后续 W4 衔接。失败/未发布/Unknown 无此资格。
不同来源或成功消耗已观测外部版本会开启新链；未观测外部修改仍由真实文件版本检查拒绝。
没有新增版本容器：记录不超过 tickets，lane 随最后记录确认回收；没有无限保留旧回执。

## 修复前真实 SDK 结果

先只增加测试，链接已安装 P05 SDK；未使用复审包中的 shim。证据见 `before/`。

| 场景 | 原实现结果 | 修复后的断言 |
|---|---|---|
| R05-01 describe 自撤销 | 0xc0000005；CDB 指向 captureSource → requestSave | STALE_SOURCE、capture=0；源/History/observed/dirty/绑定不变；重新注册可保存 |
| R05-02 accept 递归采用并确认 | accepts=2、nested_ack=1、record_alive=0；断言退出 0xc0000409 | accept=1；栈内确认 BUSY，记录仍在；外层结束后 Applied 可查并正常确认 |
| R05-04 W1 确认后请求 W4 | Scene/Material/Flow 全部失败，实际文件停在第 3 版 | 三模型实际文件均为第 4 版，最终基线/dirty 正确 |
| 不确认 W1 的对照 | 三模型原实现成功 | 继续成功 |

R05-02 的断言在外层继续写失效记录前终止，不能将该次 Windows 运行说成 ASan 报告。
本轮使用 CDB、真实 SDK、明确次数与记录存活断言；未引入 sanitizer 全平台矩阵。

## 最终验证

- 显式 `LUX_EDITOR_MIGRATION_STAGE=P05`；全量 all `-j 4 -- -k 0`，第二轮无工作。
- 完整 CTest **111/111**；原 **104** 项和原断言全部保留，新加 7 个三模型/回调场景。
- R05-01～08 全部有对应行为证据；R05-05/08 扩展原 coordinator 测试，未以数量替代断言。
- R05-03 覆盖撤销、同 Session 替代角色、describe/capture 异常、嵌套准入、Save As apply/析构回调。
- R05-06 覆盖 withRead 中 BUSY、早期确认、后续请求及反序采用，没有基线倒退。
- R05-07 覆盖真实文件别名、不同工作副本/binding、匿名来源、外部覆盖和链打断。
- R05-08 覆盖 64 次有限容量循环、取消/失败空洞、Unknown 未退休阻塞、退休后续行与完全回收。
- 原三模型、P01～P04/R1、真实 IO、17 组持久化实际依赖用例及其它已有依赖负例继续通过。
- SDK 重装，原九组消费者全部重建；共 **28** 项安装测试，包括新增真实模型 R1 场景。
- 资格绑定独立干净检出的实现 SHA，包含完整构建、CTest、安装和实际闭包。
- 原历史收据按各自 implementation_sha 核验；原 P05 快照不改写。
- 本收据可从归档目录核验；缺失/篡改必需证据必须失败，不读取生产机器旧路径。

第一次开发期依赖测试未载入 MSVC 环境，配置未产出目标图；原失败日志保留。
随后在 VS 环境重跑通过，未修改测试规则。完整最终矩阵使用正确环境。

## 文件、删除与保留边界

本轮仅修改 9 个文件，清单及 Git 内容 SHA256 见 `files.json`。
生产变化限于 SaveService.cpp、WriteCoordinator.cpp、两头文件契约注释与 persistence README。
替换了 describe 后直接解引用失效角色、无执行期保护的递归采用路径及仅凭输出回执的衔接判据。
没有移动目录、target 或包名；没有修改三类模型、History、SessionState、执行器、实际发布算法。
没有新增兼容桥。旧保存 API/SceneSaveCapture::copied 仍限定原消费者，最迟 P12 删除。
SessionStore、SessionState、WriteCoordinator 的原唯一所有权保持不变。

主工作区的 `editor/project/src/ProjectBuilder.cpp` 非本轮修改，未提交、未重置；资格检出排除它。
C01（P09/P12）、C03（P11）、C04（P12）仍为原 **FAIL**，保留日志和后续责任。
本轮 SaveService 的 B01 已单独补正，没有挂到旧 C03 延期。

既有 P05 单进程协调、乐观冲突检查、单文件发布和持久性限制不变；没有宣称跨进程 CAS、
多文件事务或新的 UI 工作流已完成。未执行 Android 构建。

首次独立全新构建中，Physics2DDescription 的编译早于静态类型头生成；原失败完整归档。
该目录及构建规则与前置验收版本相同，未在 R1 修改；生成完成后重新全量构建与二次无工作检查。
这是一项保留的冷构建顺序风险，不归入 C01/C03/C04，也不宣称本轮已修复。

验收脚本的两次配置失败也已保存：独立构建首次安装未传原前缀，以及消费者旧缓存拒绝
新的检出源码目录。分别显式指定安装前缀、使用 --fresh 重新配置后通过；没有改动生产接口或断言。
