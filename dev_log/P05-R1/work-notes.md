# P05 R1 施工记录

前置验收 5866f990f8a5c4d68e19e0e395c9487c6c0f2779，分支 codex/editor-redesign-v4；仅 P05 R1。
原 P05 的 104 项与断言保留，dev_log/P05 不修改。C01/P09+P12、C03/P11、C04/P12 保持原 FAIL。

## 修复前真实 SDK 证据

先仅向公共 API 消费者加入测试，链接已安装 P05 SDK；没有使用复审包 shim。
r1-revoke 退出 0xc0000005，CDB 栈 captureSource -> SaveService::requestSave。
r1-recursive 输出 accepts=2/nested_ack=1/record_alive=0，断言退出 0xc0000409。
r1-chain 三个实际模型 W4 均未发布，文件仍是第 3 版本，断言退出 0xc0000409。
r1-chain-control 三模型成功。before/ 保留命令、返回码、日志、文件、测试补丁及 SDK 哈希。

## 最小状态及责任

SaveService::Impl 一个 dispatching 由私有 DispatchScope 在 owner 栈内持有，覆盖回调及清理。
递归变更返回 BUSY，adoptCompletions 递归不进入，status 与 token 撤销可用。不是 Session busy。
describe/capture 返回后检查原 registration 的 source；不切换到新角色。拒绝回收票据和捕获额度。
已进入 accept 可完成；撤销只禁止后续调用，不篡改 Published。
WriteCoordinator 复用现存成功 Record.work.target.expected_version 作为已验证消耗前提，
只有当前同目标/合法 Session+Binding/chain_begin/前序 ticket 中成功 receipt 的边可衔接。
新成功发布若消耗版本不是先前受控版本，即开始新链（含已观测外部修改）；外部未观测修改仍冲突。
无新增无限表；最多 tickets 个 record 与 lane，确认实际删除 record，最后一条确认释放 lane。

## 范围

生产代码仅 persistence 两实现和两头的契约注释及 README。三类模型、History、SessionState、
执行器和物理发布算法不动；无目录、target、包名改动，无新增兼容桥；既有旧 API 仍 P12 删除。
工作中出现的 ProjectBuilder.cpp 用户改动已记录指纹，不纳入本轮提交，也不重置。
最终验收须使用干净实现 SHA 的独立检出，以免将该未提交修改混入资格结果。

最终实现 1b2366616ba918bba29a3dcd1349274c574de398 在前一实现上仅按 AGENTS 将拒绝条件具名。
资格复用原独立干净 clone/build 目录但 fetch/checkout 最终 SHA，重新 tracked 检查、配置和全部矩阵。
初次全新 all 构建的 Physics2DDescription 缺生成头失败保留；头随后生成，未修改 engine/CMake。
该冷构建顺序风险独立记录，不归入三项原缺陷，不声称通过本次保存服务补正解决。

最终完整 CTest 111/111 与全部详细 R1 回归通过。独立 clone 的安装脚本初次未显式传前缀，
尝试默认 Program Files 被拒绝；修正验收命令使用原 SDK 前缀，保留原日志，不改生产实现。

安装消费者切换到独立检出的源码后，旧 CMakeCache 拒绝更换 source directory。
为 Scene/Material/Flow 三组指定 cmake --fresh 重建配置，原失败日志保留；不修改消费者源码或断言。
