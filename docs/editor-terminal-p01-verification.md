# Editor 终态规范：P01 ObjectTarget / ObjectScheduler 验收

本轮只实施《LuxEngine Editor 终态架构与实施规范》第 6 节 P01。阶段门禁通过后停止复审，
没有进入 P02，没有提前修改 ProjectTransition、TaskScope 析构、Context、Registrar、Root 或宿主循环。

## 固定版本与范围

| 项目 | 值 |
|---|---|
| 前置收据 | `af07faa7e08b1cb3e9022328dbc433d4dd775cf3` |
| Object / scheduler 实现 | `3d34b0af8f95a28a1251e196d2e1f2e81b3f2ecc` |
| 依赖验证补正及最终资格 SHA | `865ee8861859b3fb0dbada1290731294be2cd58a` |
| lux-cxx，未修改 | `0a0e7419fc7229df6e372cd35a540249f92250ef` |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| 新安装 SDK | `E:/SyncForder/CodeRepos/install/Framework-terminal-p01` |
| 构建 | Windows x64、MSVC 19.44.35228、C++20、RelWithDebInfo、无 RTTI |

最终资格从最终实现的另一份干净检出开始，先执行 `ValidateTrackedSnapshot`，重新冷构建。
不使用用户排版差异或旧 SDK 补齐源码。实现与本记录分别提交；本记录的 Git 提交即 qualification commit。

## 实际文件与责任

新增 11 个文件：

- `modules/core/object/include/lux/engine/object/ObjectTarget.hpp`
- `modules/core/object/src/ObjectTarget.cpp`
- `modules/core/object/test/target.cpp`
- `modules/core/object/test/target_contract.py`
- `engine/process/object_execution/CMakeLists.txt`
- `engine/process/object_execution/README.md`
- `engine/process/object_execution/include/lux/engine/process/ObjectScheduler.hpp`
- `engine/process/object_execution/src/ObjectScheduler.cpp`
- `engine/process/object_execution/test/ObjectSchedulerTest.cpp`
- `engine/process/object_execution/test/check_dependencies.py`
- `cmake/installed-consumers/object-execution/CMakeLists.txt`

修改 7 个文件：`engine/process/CMakeLists.txt`、`modules/core/object/CMakeLists.txt`，以及 Object 模块的
`LuxObject.hpp`、`detail/MessageEnvelope.hpp`、私有 `detail/ObjectState.hpp`、`Object.cpp`、`ObjectRuntime.cpp`。
本阶段没有删除／改名文件，没有为后续阶段提前删除公开协议。

唯一队列与 owner 仍是原 ObjectRuntime。ObjectTarget 复用 `ensureState()` 与原 intrusive ObjectState，
不保活 LuxObject，不增加 weak registry、线程池或队列。`object_execution` 是独立 STATIC 桥，
依赖原 Process、core Object 和 stdexec；core Object 不反向依赖它们。

删除了 producer 在 post 后发现关闭便自行丢弃已接受队列的分支。现在只有 owner 派发／结清已接受工作；
进程关闭仍要求先停止并 join 原生产者。MessageEnvelope 显式区分需要关闭完成的消息，普通 Signal
保持原丢弃语义，不通过检测任意 callable 的同名方法改变它的行为。

## 新不变量与测试意义

| 观察项 | 真实生产入口与断言 |
|---|---|
| O01 | 实际 `stdexec::schedule` / `continues_on`，live target 恰好一次 value |
| O02 | 派发前销毁实际 LuxObject；stopped，用户 then 不执行；后续槽位复用不能恢复旧 target |
| O03 | 派发前 `beginDestruction()`；仍存活的关闭对象也不接收业务 continuation |
| O04 | 用公开 post 填满原有界队列；立即 `CAPACITY_EXCEEDED`，不等待、重试或加队列 |
| O05 | start 前 stop，不增加 posted 计数 |
| O06 | POSTED 后从 worker stop；消息仍在队列，原 envelope 派发时一次 stopped |
| O07 | worker start 及实际 `ExecutionRuntime::blocking()` → ObjectScheduler，value 在 owner 线程；同步 continuation 中结构改动返回 BUSY |
| O08 | 100 批、总计 10,000 次调度，固定随机种子销毁部分目标；6662 value + 3338 stopped，每项恰好一次，末尾无 pending envelope |

`object.target` 另验证 worker 投递、派发内新投递留到后批、关闭后的 nullptr，以及拥有载荷在派发后释放。
`object.target_contract` 以真实进程验证：回调内销毁被借用目标必须失败；Runtime 最终关闭时，已接受
completion 以 nullptr 恰好结清一次，新的 post 返回 CLOSED。原 queue / shutdown / ownership / services
测试保留，普通 Signal 没有新增关闭回调。

Scheduler stop callback 在终态交付前撤销；调用 receiver 后不再访问 operation。另有 receiver 当场
delete 实际 operation 的测试。POSTED 返回路径同样不再访问 operation，允许 owner 已经并发交付完成。
拒绝／start 前停止可以在调用线程完成，因此只有 set_value 宣告 Object completion scheduler。

ObjectState 的引用与释放沿原 intrusive_ptr；没有新增循环拥有关系。上述压力结果是普通优化构建的
行为资格，不冒充 sanitizer／堆泄漏检测器成绩。规范第 40 节的 ASan／UBSan 资格仍留待 P06。

## 工程资格

命令统一前缀为 `terminal-p01-final-`，完整参数、stdout/stderr、时间、Git SHA、dirty 状态及哈希在外部归档。
全部构建使用 `--target all -j 4 -- -k 0`，构建与 GPU／桌面验证串行。

| 验证 | 实际结果 |
|---|---|
| Editor | 冷构建 1091 步，第二轮 no work；完整 CTest **55/55** |
| PLAYER | 冷构建 1021 步，第二轮 no work；完整 CTest **38/38**，不依赖 Editor |
| 既有安装消费者 | 全新 SDK 构建／no work；**16/16**，包含跨 DLL、实际桌面／GPU、项目切换和回调约束负例 |
| 原最小消费者 | Project、通用 Scene IO、TaskScope 各 **1/1** |
| 新最小消费者 | 仅 Object **2/2**；仅 object_execution **1/1**；均二次无工作 |
| 安装公共头 | 原 38 个加 2 个新增头，独立 C++20／无 RTTI 编译 |
| 桌面／GPU | Editor 完整矩阵中的 5 个 GPU、4 个 desktop 标记测试通过；安装矩阵的实际 GPU／桌面通过，未以 mock 替代 |
| 安装产品 | 中文路径 create、重开清单字节保持、WM_CLOSE 正常退出；没有接管鼠标键盘 |
| 依赖负例 | 实际 CMake 同夹具合法→Object 到 Process／bridge 到 Editor／bridge 到 UI 非法边→指定规则拒绝→删除边恢复 |
| 负例恢复 | 恢复后的全量构建通过，第二轮 no work；未保留非法边 |
| 安装闭包 | 新 SDK 无 legacy、旧 SDK／源码私有头补齐；Object-only 不导入 Process/stdexec，bridge 不导入 Editor/UI；STATIC 无新增桥 DLL |

三个 Object 公共头已同步 Debug、RelWithDebInfo、Android 的规定 include 前缀，逐项哈希记录保留。
这不是 Debug 或 Android 构建资格；本轮未改 lux-cxx。

## 静态范围、失败与延期

`static-audit` 核验最终提交相对前置：`editor/`、`engine/process/execution/`、`engine/context/`、
`modules/function/ui/` 完全未变。core Object 中没有 stdexec／Process／Editor include；生产
ObjectScheduler 没有 wait／spin／retry 或 Editor/UI include。

`ProjectTransition`、`advanceProject`、`retiring_panes` 仍有生产引用，准确路径已归档。
它们属于后续阶段删除项，不能在 P01 报告终态 grep 清零，也不能提前删掉现有正确关闭责任。

保留全部首次失败：重复 stdexec 导入；Windows post 导出／friend 声明不一致；测试误把
ExecutionRuntime value 当指针；首次冷构建后依赖检查误把 build/editor 当源码 Editor（**54/55**）。
最后一项已改为实际 File API 源码／provider 归属，并为 PLAYER 独立请求 codemodel。另起最终干净检出
重新构建／运行，未覆盖首轮输出。最终 P01 非延期必测无未解决失败；原 LLVM 转换及测试 NDEBUG
覆盖等构建警告没有被改写成零警告。

原生输入仍为 **NOT_RUN_USER_DEFERRED**；Linux、系统 IME、历史性能延期不变；ASan／UBSan 本轮
未运行，P06 仍有明确责任。没有 Android 构建、旧长测或具体编辑工具迁移。旧历史 FAIL/PARTIAL
与免验范围不变。

## 归档、用户差异与停止点

唯一可变材料：`.internal/editor-redesign/terminal-architecture/`。
外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p01/verified-evidence/`。
冻结 1384 份文件、67 条已执行命令（包含失败）；验证只读取相对归档文件与固定 Git 对象。
中文／空格路径搬迁通过，删除或篡改真实 SDK 结果均拒绝。
归档 manifest SHA256：`77be86228cf914a430f91a122bb2db86c31d58e38829d9178541e6f8b5882e51`。
冻结／验证本身的输出保存在同一外部 runs 目录；历史归档没有改写。

三处用户排版差异（LuxEngine.cpp、EditorContext.hpp、Pane.hpp）保持原字节、未纳入提交。
ProjectBuilder 补丁仍独立、未应用，原 SHA256 为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
两个临时 qualification 检出在取证验证后清理，主工作区、开发构建、SDK 与历史保留。

**P01 Gate：PASS，STOP / review。P02 未开始；没有合并 main、删除历史分支或发布 release。**
