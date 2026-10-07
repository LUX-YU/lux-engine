# Editor 终态规范：P06 Host loop 与最终关闭

P06 功能、静态及 Windows 工程门禁：PASS。ASan：PASS（覆盖范围见下文）。
UBSan：NOT_QUALIFIED；不宣称完整 sanitizer 资格。按规范 §40 明确保留当前平台限制。
按用户授权继续 P07，不等待中间复审；后续 LR/MA 仍按各自技术门禁实施。

生产重构 `38048ae2aa91fe42491cffaad5de229798d8b8e4`，最终实现及资格补正
`3f3060d6b344d480844028dbbe7d97156016db94`；前置 P05 验收 `1cd0fbc0f`。
本文件为独立验收提交，不继续修改生产实现。

## 实际变化

公开入口统一为 `run()`，单步推进仅在未安装的 private test support 中可用。删除公开
frame/exec、FrameStatistics 与逐帧生产计时。EditorWindow 缓存窗口、像素尺寸和最小化事实，
主循环只在 revision 变化时同步尺寸，每轮保持一次 Input sample 和一次 SceneRuntime drive。
不存在 Project transition、Context 退休列表、Project 生命周期轮询或 Context/Pane 析构等待。

旧安装 SDK 的真实负例：后台已向 ObjectScheduler 投递完成，直接销毁宿主持续等待并触发
watchdog。补正仅在最终应用边界停止准入，继续原 Object/Process 完成接收，再由 Engine
终止并 join 生产者。没有修改 TaskScope 或 Process 调度算法。HostResources 的 RAII 顺序
保持 UI Scene、Engine、原生窗口依次释放；LuxEngine 在派生成员消失前撤销接收资格。

## 最终提交上的验证

独立 clean tracked 检出，ValidateTrackedSnapshot 通过。Editor/PLAYER 初始冷构建绑定
`38048ae2a`；最终提交在同一独立检出增量复验，不把初始冷构建冒充最新提交的冷构建。

| 范围 | 结果 |
|---|---|
| Windows Editor | all -j 4 -- -k 0，第二轮 no work，62/62 CTest |
| Windows PLAYER | all、no work，40/40 CTest |
| MSVC ASan | all、no work，62/62 CTest |
| 重装 SDK | 16/16，公共头逐个 C++20/无 RTTI 编译 |
| 独立安装消费者 | app、Project、Scene、services/tasks、TaskScope、ObjectScheduler 各 1/1；Object 2/2 |
| 依赖正反例 | Context→UI、UI→Editor 被指定规则拒绝；去边恢复，恢复后 all/no work |
| 真实呈现/文件 | UI GPU、resize/minimize、项目替换、迟到完成、已投递完成后关闭；安装产品中文路径创建/重开/WM_CLOSE |
| 证据 | 中文/空格路径搬迁通过；缺失和篡改真实 SDK 日志均拒绝 |

SDK 为 `E:/SyncForder/CodeRepos/install/Framework-terminal-p06`。实际 source/include/link/install
闭包未借用旧 SDK、legacy 或源码私有头。安装消费者通过公开 run/Events 验证；原细粒度
frame、来源、回调、快捷键和 safe-point 断言留在源码 private support 测试，不以数量替代语义。

## 首次失败与 sanitizer 范围

最初 public_host 夹具丢弃 Task handle，触发原 RAII 取消/放弃回调；改为真实 TaskScope owner，
保留首次失败。MSVC ASan 首轮 61/62 命中 object.target 夹具：内层排队闭包借用了已经结束的
worker 闭包中的 endpoint。`b4bf8c361` 改为按值捕获，保留全部断言；最终提交完整重跑通过。

ASan 覆盖第一方生产及测试目标，外部预编译依赖未插桩。LLVM/shaderc/spirv-cross 的 STL
注解链接不一致按 [Microsoft 合同](https://learn.microsoft.com/en-us/cpp/sanitizers/error-container-overflow?view=msvc-170)
统一关闭 vector/string size-capacity 注解；普通堆/UAF 检测保留。不声称容器注解资格。

Clang ASan 的 Windows interceptor 启动失败保留。Clang UBSan 独立 runtime 与动态 CRT 冲突后，
尝试正式 trap 模式；完整构建在 `736e2e9d9` 完成，但真实测试失败。cdb、IR 和无 Lux 头的
普通 C++ 空基类复现定位了 object-size/MS ABI 问题。该诊断探针不是产品通过证据。

进一步启用真正的 Clang 无 RTTI 后，最终构建被原 `CodeLease.cpp` 的
`std::get_deleter<CodeOwner>` 阻止：当前 MS STL 在该模式删除此接口。问题编号
**Q-P06-CLANG-GET_DELETER**，纳入后续机制审计；不得牺牲代码 pin/析构尾部或无重复包装语义。
完成前不宣称严格 Clang 全量及 UBSan 资格。没有为取得绿色结果打开 RTTI 或删除危险断言。

本次实际修正两处 Script C++20 可移植性错误：将 incomplete nested type 的原 traits 断言
移至完整类型可见的构造函数体；歧义的 `{}` 赋值写明既有 PreparedResumeType。没有改执行算法。
无 RTTI 门禁区分 MSVC 与 clang-cl，后者使用 `/clang:-fno-rtti` 并验证功能宏；真实 stdexec
头正例和非法 cast 负例通过，完整 Clang 构建限制仍如上记录。全部首次失败保留。

## 归档与保护

证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p06/verified-evidence/`。
1572 个文件、164 条真实命令；manifest SHA256：
`08e8d17586b4306f013d1d76020286d9809fb2a3a8ffb4c97c4a418cd13bd067`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。

Context/Pane 用户排版差异仍未提交。LuxEngine 旧 profiling 原体按规范删除，原字节和用户 patch
保存在 terminal-p05/protection；不声称当前实现与旧补丁逐字一致。ProjectBuilder 补丁未应用。
main、历史判定不变。原生输入 NOT_RUN_USER_DEFERRED；Linux、IME、旧性能延期在终态阶段
仍按原记录。后续 LR/MA 的新资格要求独立执行，不能用本记录代替。
