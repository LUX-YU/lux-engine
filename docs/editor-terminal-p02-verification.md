# Editor 终态规范：P02 TaskScope 验收

P02 gate：PASS。按用户最新授权继续 P03–P07，不在阶段间停下等待复审。
P01 既有验收不改写；本阶段没有改动 ObjectScheduler、Context、ProjectTransition、Root 或 main。

## 固定版本与责任

- 前置：`9ee997251db4fc5fd211b2c23bf8d915003d2e21`。
- 实现：`d9d3f6907d6f604b4b13295509fcd044935c6698`；本记录单独提交。
- lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`，未修改。
- 工作区：`E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。
- 新 SDK：`E:/SyncForder/CodeRepos/install/Framework-terminal-p02`。

TaskScope 和每个已接受 TaskRecord 共同拥有同一 TaskGroup。Scope 析构仅 requestStop；
最后一个 record 消失后自然释放 group，没有退休表、额外队列或关闭状态机。
原 collect 次序保留：交付 completion、销毁 operation/code owner、结清 outstanding、退休 record。
join 保留为显式同步边界，不再由 TaskScope 析构调用。

迁移 TaskScope.hpp、Task.hpp、私有 TaskState.hpp 和 Task.cpp。VfsAssetReadEndpoint 的完成回调
改为唤醒原 ExecutionRuntime，不再经可能已析构的 TaskScope；准入仍借用 scope，必须先撤销端点准入。
RenderResources、WorldLoading 的结果 owner 审核通过，未复制其算法。脚本既有显式同步边界保留，
本阶段没有将其扩展为 Project/Pane 析构屏障。

## 真实修前／修后与行为

同一个 TaskScopeTest 先通过实际安装的 P01 SDK 编译运行。worker 被 semaphore 阻塞，Scope 析构
等待；两秒 watchdog 释放 worker 后明确报 `FAIL T01: TaskScope destructor waited for the blocked worker`。
该失败输出和测试原文冻结保留，未用简化 shim 代替。

新实现面对同一阻塞立即返回，且 worker 尚未交付。测试继续创建、销毁 128 个 scope，随后释放
worker，验证 stop token、progress、phase、拥有型输入、owner 线程完成、恰好一次交付及 code/payload
释放。另验证 Runtime 最终排空、显式 join，以及 completion 执行期间 settled 尚未提前变真。
实际 VFS 测试覆盖端点准入 scope 消失后的完成。原行为断言保留。

## 工程结果

从固定实现的干净独立检出执行 ValidateTrackedSnapshot，然后顺序构建与运行，没有与 GPU 并发。

| 项目 | 实际结果 |
|---|---|
| Editor | 全量 all -j 4 -- -k 0、第二轮 no work；56/56 CTest |
| PLAYER | 全量构建、第二轮 no work；39/39 CTest |
| 新 SDK 原消费者 | 构建／no work；16/16，包含真实桌面和 GPU |
| 最小消费者 | Project、Scene、原 services/tasks、ObjectScheduler、新 TaskScope 各 1/1；Object 2/2 |
| 公共头 | 原 38 个及桥的 2 个独立 C++20／无 RTTI 编译 |
| 实际依赖负例 | Object→Process、桥→Editor、桥→UI 均准确拒绝；同夹具去边恢复，通过全量/no work |
| 安装产品 | 中文路径 create/reopen、清单保持、WM_CLOSE 正常退出；未接管原生输入 |
| 闭包 | 新前缀无 legacy、源码私有头或旧 SDK 补齐；安装头与实现一致 |

静态核验确认 editor、engine/context、core/object、function/ui 相对前置未修改；
ProjectTransition/advanceProject/retiring_panes 此时仍存在，属于后续阶段，未提前宣称终态清零。
本阶段没有修改 modules 公共头，不产生新的三个安装 include 同步责任。

## 证据与保留范围

唯一施工节点仍为 `.internal/editor-redesign/terminal-architecture/`。
外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p02/verified-evidence/`。
包含 946 份文件、61 条实际命令（包括修前失败），manifest SHA256：
`4da8c9a219495c2eb9d9bcb16238af07c07178c68396616bd9119132a23ed010`。
中文／空格路径搬迁验证通过；缺失或篡改实际 SDK 日志均被拒绝。验证不依赖生产日志绝对路径。

原三处用户排版差异未纳入实现；ProjectBuilder 补丁独立、未应用，原 SHA256
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
原生输入仍 NOT_RUN_USER_DEFERRED；Linux、IME、历史性能、既有免验不变。
本阶段不把压力测试当成 sanitizer，P06 的 ASan／适用 UBSan 责任仍保留。
未修改 main、历史快照或发布 release；后续按原规范推进。
