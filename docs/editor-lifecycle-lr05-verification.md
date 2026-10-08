# Lifecycle LR05：端点关闭准入，任务独立结清

固定生产实现 `a58e3424bfa126907fb3f0d98a6846bc170f23ef`，lux-cxx
`0a0e7419fc7229df6e372cd35a540249f92250ef`。LR00 冻结的 18 条 C4 责任、三个提供者已逐项核对。
LR05 迁移完成，Windows 及本阶段安装/GPU 验证通过。本记录仅覆盖 LR05，不宣告整个 LR/MA 计划完成。

## 所有权与删除

| 提供者 | 当前责任 | 删除与保留 |
|---|---|---|
| VfsAssetReadEndpoint | 唯一公开 owner 控制准入；复制 port 只保留私有 gate/容量计数，已接纳任务持有读取快照、scheduler、输入与完成 | 删除公开 requestStop/join、EEndpointState、shared_from_this facade 和 submit 继承；保留原限额、错误、关联及取消 |
| ScriptRealDelayProvider | 唯一 provider 拥有原 TaskScope；任务只拥有 Request 完成与终态，析构立即撤销并请求取消 | 删除 ACTIVE/STOPPING/JOINED provider phase、公开 stop/join 和无生产者错误；原 Timer 错误、容量及 BACKPRESSURE 保留 |
| ScriptAssetAccess/Scope | Access 撤销原 TaskScope 借用并释放读取端口；Scope 保留原代际身份及有界结果，任务持有独立状态与代码 | 删除阻塞 Impl 析构、重复 stopping 与 Reservation::state；保留显式 scope revoke、限额、结果确认与原 Runtime 物理屏障 |

三个实现提交依次为 `f986a6423`、`20d6eae75`、`a58e3424b`，合计 15 个生产/测试/构建文件。
没有新增端点退休队列、公共 closing phase 或完成管理器。ScriptSystem 先撤销 invocation 和借用，
再按原成员 RAII 释放 provider；普通语义析构既不 join，也不收取其他任务的业务完成。

回调可以删除当前公开 owner。同步调用只临时持有其所需私有存储；这些引用不延长公开对象寿命，
也不恢复准入。后台任务不捕获整个 provider/Impl。VFS 的 TaskScope 借用只覆盖准入调用；
ExecutionRuntime 必须覆盖已接受任务，最终屏障仍属于该 Runtime。

## 行为证据

- 实际旧安装 SDK：VFS facade 被阻塞请求保活；延迟 provider 析构收取无关完成；资产 Access
  析构等待 watchdog 释放真实 CPU worker。三份失败输出及原输入均保留。
- VFS：阻塞读取时 owner 死亡、复制端口拒绝、排队取消/Scope 析构、完成/醒来/清理重入、容量复用。
- 延迟：容量 32、真实 Timer 拒绝和取消、背压重试、递归 drain、完成和 wake 中删除 owner。
- 资产：raw/typed × CPU/IO 阻塞 × Scope/Access 析构八种组合；完成、active 查询、wake、重试和
  结果/provider 清理中删除 owner；撤销后零保留预算、旧身份拒绝、任务正常结清。
- 真实插件 DLL：Access 在结果派发前析构，资产和最后代码 owner 仍保留；正常收取后在正确线程
  析构资产并卸载 DLL，一次释放。未以声明级或隔离 shim 替代。

原危险断言保留。修前夹具漏传空 fallback、显式 AssetEncodeLimits、delay 字段名/显式 join
前未 settlement，以及 Windows CMake 路径转义的首次失败均保留；修正夹具的准确原因，没有放宽生产语义。
ScriptAssetAccess 原 README 的 join-on-destruction 描述在独立文档提交更正。

## 固定提交资格

先 ValidateTrackedSnapshot，从独立 clean tracked 检出、全新 build 与 SDK 前缀验证。
构建和桌面/GPU 运行串行。

| 验证 | 结果 |
|---|---|
| Editor 冷构建/第二轮/完整 CTest | 1229 步 PASS / no work / 111/111 |
| PLAYER 冷构建/第二轮/完整 CTest | 1132 步 PASS / no work / 54/54 |
| 安装框架消费者 | 192 步 all、no work、16/16 |
| 本阶段安装消费者 | 29 步 all、no work、VFS/delay/assets/真实插件 4/4 |
| 修改的公共头 | 三个独立 C++20/noRTTI 编译，clean source 与全新 SDK 字节一致 |
| 实际 source/include/link/install 闭包 | Editor 650、PLAYER 594、框架 SDK 59、本阶段 SDK 8 个 TU；无 legacy/旧 SDK/源码私有头回退 |
| 桌面/GPU | Editor 完整测试中 34 个 gpu、8 个 desktop 标签项通过；不等于原生输入/IME 资格 |

三份早期收据的 73 个声明项按各自实现 SHA、Git 对象和输出哈希核验。scoped ASan 覆盖原
VFS、delay、assets 实现及测试；依赖 DLL 未全部插桩，此处按原收据继承，不能称本次重跑或全后端资格。
LR05 只改变 engine 公共头，没有 modules 公共头变更；三前缀同步规则不触发，未执行 Android 构建。

## 归档与剩余范围

外部归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr05/verified-evidence/`。
1137 个文件、66 条命令、19 条最终必需命令。Manifest SHA256：
`34c2e2d1994d18a2bad8fdd5aa7d2e125c2e2747d80a45cdbcffe02d1c279ce6`。
归档复制到中文/空格路径后完整验证通过；移除和篡改真实 SDK 输出均被拒绝，没有生产机绝对路径回退。
逐条冻结范围、原源码 blob、所有首次失败、实际消费者清单、构建/链接/安装输入均在同一相对归档中。
搬迁及破坏性负例输出另保存在同级 `archive-verification.log`。

`Q-LR03-HOST-MINIMIZE` 保持 OPEN，本次通过不证明根因已修；已知蒙皮跨帧 WAR 责任不变。
原生输入继续 `NOT_RUN_USER_DEFERRED`，IME 未测；Linux 和支持的平台 sanitizer 仍为 LR08 必测，
不新增免验，不恢复旧性能长测。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。六处用户差异哈希未变，
ProjectBuilder 补丁未应用，main 和历史记录未改。后续继续 LR06 的原 27 条责任，不等待阶段复审；
MA 尚未开始。
