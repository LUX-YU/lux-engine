# Lifecycle LR04：注册责任随唯一 lease 结束

固定实现 `f0930784cadadaf04a8c5a1ba517a6696357b792`，lux-cxx
`0a0e7419fc7229df6e372cd35a540249f92250ef`。LR00 冻结的 17 条责任已逐项核对；
VFS、Input、vertex-source 注册迁移完成，Windows 与本阶段安装/GPU 验证通过。
这不是整个 LR/MA 计划完成，也不关闭历史未解决问题。后续继续 LR05，不等待阶段复审。

## 唯一责任与删除

| 范围 | 唯一 owner 与寿命 | 删除/迁移 |
|---|---|---|
| VFS | `MountLease` 拥有一项登记及原 VFS 控制状态，不保活 AssetVfs；已捕获快照与已接受读取持有 provider | 删除 raw-ID `unmount`；`replaceMounts` 一次发布完整候选，成功后才清空旧 lease |
| Input | `InputContextActivation` 唯一拥有固定地址记录；Context/Stack 只索引它，任一端析构撤销两侧关联 | 删除 push/pop、enabled 和可变 priority；优先级由 activation 固定，ActionMapper 保留原消费与 UI capture 语义 |
| vertex | `VertexSourceRegistration` 唯一拥有固定地址记录；Registry/source 只借用索引 | 删除 raw-slot unregisterSource/refreshSource；StaticVertexPoolSet 与 SkinningResources 持有实际 lease，无手工注销循环 |

全部 lease 不可复制，可 noexcept 移动；移动后原值为空，移动赋值先释放旧责任，自赋值安全。
source/registry 先于 lease 消失、同地址与槽位复用均有实际测试。IVertexSource 基类析构撤销登记时
不调用已析构的派生虚函数；生产聚合对象按成员顺序先释放登记，再释放来源及 native backing。
GPU 安全点仍由原 Renderer/Scene/ResourceRegistry 负责，未新增退休队列或 GPU 句柄代际格式。

实际修改 30 个文件，包括三个提供者、全部实际调用者和对应测试；逐文件差异及 17 行映射位于
归档 `changed-files.tsv`、`inputs/frozen-coverage.json`。三个实现闭包分别为
`ad7eb6dc5`、`fa9019f5b`、`30725b74c`，真实生产着色器回归为 `f0930784c`。

## 固定提交验证

先 ValidateTrackedSnapshot，再从独立 clean checkout、全新构建树及全新 SDK 前缀验证。
构建和 GPU 执行串行；原危险断言保留，未以测试数量替代责任核对。

| 验证 | 结果 |
|---|---|
| Editor 全量/第二轮/CTest | 1223 步 PASS / no work / 108/108 |
| PLAYER 全量/第二轮/CTest | 1126 步 PASS / no work / 51/51 |
| 安装框架消费者 | 192 步 all、no work、16/16 |
| 本阶段安装消费者 | 实际 VFS mount 与 Input activation/ActionMapper 寿命测试通过 |
| 修改的公共头 | 5 个独立 C++20/noRTTI 编译；Debug/RelWithDebInfo/Android 三前缀逐字节一致 |
| 真实闭包 | Editor 647、PLAYER 591、SDK 59 个 TU；PLAYER 无 Editor，SDK 无源码私有头/旧 DLL 回退 |
| 实际 vertex GPU | 原 skin_compute SPIR-V、原注册池与 SkinningResources；两帧变换结果读回、fence 后撤销、验证层错误为零 |
| scoped ASan | 三批原实现收据按原 SHA 继承：实际 VFS/VirtualPath、Input/ActionMapper、vertex/Skinning/mesh 实现通过；不冒充本次重跑或整个后端 DLL 插桩 |

GPU 回归使用第二次原生产 shader dispatch 读取真实注册的蒙皮输出池，再写入可读回缓冲；
不是只验证 key 或映射参数，也没有为测试改生产 buffer usage。它不代表完整 SkinningFeature
渲染图资格，更不代表修复跨帧 skinned transient output 的 WAR 问题。

## 修前证据与首次失败

- 旧实际 SDK 的 VFS 登记离开结果作用域后仍保留 mount/provider；Input 的目标析构和优先级变化
  违反新合同；实际旧 vertex Registry 析构后 source 仍显示 pool 0。原失败输出及输入保留。
- 首次夹具缺少 stduuid include、ActionBinding 前置声明遗漏、ASan 夹具重复编译 production CPP、
  GPU 夹具误复制 LogicalDevice 的失败均保留，分别修正真实原因，未删除断言。
- 最终公共头检查首次发现 Debug 安装头为 LF、干净 Git 检出为 CRLF；规范化内容相同。保留首次
  失败记录及哈希，从固定 clean source 重新同步五个头后，原严格字节比较通过。未放宽验证器。

三份早期实现收据共 147 个声明项按原日志哈希与 Git 对象核验；该完整性检查不计为重跑。

## 归档与限制

外部归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr04/verified-evidence/`。
共 1230 文件、81 条命令、24 条最终必需命令；manifest SHA256：
`b563351c6053c80fa08135b993ad2612e6912277a9af2467738e4aba5537bc8e`。
中文/空格路径搬迁通过；删除或篡改真实 SDK 输出均被拒绝，无生产机器绝对路径回退。

`Q-LR03-HOST-MINIMIZE` 仍 OPEN，本次 clean/SDK 通过不构成根因修复。已知蒙皮跨帧依赖问题
保持原责任。原生输入继续 `NOT_RUN_USER_DEFERRED`，IME 未测；Linux 和支持的完整 sanitizer
资格保留为 LR08 必测，不制造免验；Android 仅同步头，未恢复旧性能长测。

工作区 `E:/SyncForder/CodeRepos/lux-engine`、分支 `codex/editor-framework-v2`。
六处用户差异哈希不变，ProjectBuilder 补丁未应用，main 和历史记录未改。
LR05 的原 18 条 C4 责任已冻结，仅完成源码审查及尚未运行的负例准备；MA 尚未开始。
