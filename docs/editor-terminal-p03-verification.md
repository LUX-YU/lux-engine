# Editor 终态规范：P03 composition / Context 验收

P03 gate：PASS。按用户授权继续 P04–P07，不等待单阶段复审。

## 固定版本和实际迁移

- 前置：`fc39d6eb3`（P02 独立验收）。
- 实现：`d2dfe28092165e352dd2357ee595faacc308f79c`；本记录独立提交。
- lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`，未修改。
- 工作区：`E:/SyncForder/CodeRepos/lux-engine`，`codex/editor-framework-v2`。
- 新 SDK：`E:/SyncForder/CodeRepos/install/Framework-terminal-p03`。

EditorAssembly 只构建 EditorComposition。四份注册数据移入 EditorServices、EditorUiRegistry、
SceneProfileRegistry 和 SceneToolRegistry；删除原四个 Registrar 的头、实现和全部活动调用。
运行期没有注册、freeze、assembled 或 closing 标志；服务仍采用小 vector，惰性唯一构造并逆序销毁。
销毁服务之前移出查找表，因此用户析构不能重新取得或构造服务，没有增加另一份关闭状态。

Context 只从完整 PreparedProject 和 composition 创建，PluginManager / SceneRegistrations 为有效引用。
直接 TaskScope 是最后一个成员，默认析构只撤销准入和请求取消，不等待 worker。
公开的不完整 create、beginClose、closed、closing、freeze 及旧 nullable capability 均已删除。
SceneToolRegistry 保留既有 provisional 匹配语义，未扩大其冻结范围。

createPane 的 UI 执行仍属于 lux_editor_ui。Context 返回不可修改的工厂记录借用，既不实例化 Pane
析构，也不反向链接 UI。首次使用 function_ref 表达该返回值触发 MSVC 的 incomplete Pane 实例化失败；
原失败保留，修正为同一记录上的窄 createPane 访问，不增加第二套工厂或共享控制壳。

旧 ProjectTransition 仍存在，调用方只机械迁移为 tasks().requestStop()/settled()，待 P05 删除。
本阶段没有修改 Process、ObjectScheduler、Root 或其他 modules 算法。

## 行为与工程证据

真实 CPU Context 测试构造完整插件与 SceneRegistrations，验证不完整输入、assembly 失败和完整引用。
worker 被 semaphore 阻塞时，Context 析构先返回；取消可见，拥有型输入继续存活，释放 barrier 后完成
仍在原 owner Runtime 收取且只交付一次。首次夹具遗漏 blocking scheduler 的失败保留，补齐真实配置后通过。
源码测试使用不安装的完整准备夹具；安装消费者经产品真实打开路径，不使用私有头或测试构造入口。

从固定实现的独立干净检出运行 ValidateTrackedSnapshot；构建和 GPU/桌面测试串行。

| 项目 | 实际结果 |
|---|---|
| Editor | 全量 all -j 4 -- -k 0，第二轮 no work；57/57 CTest |
| PLAYER | 全量构建、第二轮 no work；39/39 CTest |
| 安装 SDK | 全新前缀；原 16/16 消费者，包括实际项目打开、桌面/GPU 和 Context-only 链接 |
| 最小消费者 | Project、Scene、services/tasks、ObjectScheduler、TaskScope 各 1/1；Object 2/2 |
| 公共头 | 当前清单逐头 C++20、无 RTTI 编译；不从源码私有头补齐 |
| 依赖负例 | 实际给 lux_editor_context 添加 UI 边，检查准确拒绝；同夹具去边恢复，all/no work |
| 产品 | 安装 lux_editor 在中文路径 create/reopen，清单字节保持，WM_CLOSE 正常退出；无输入接管 |
| 删除闭包 | 新前缀无旧 Registrar、PreparedProject/fixture、legacy 或旧 SDK 回落 |

安装 Context / profile 消费者现在使用真实宿主，涉及桌面及 GPU；未将其描述为纯 CPU 测试。
P01/P02 危险断言和既有 Context/服务/UI/profile 断言保留，没有按相同测试数量替代语义。

## 归档与保留范围

唯一施工材料：`.internal/editor-redesign/terminal-architecture/`。
证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p03/verified-evidence/`。
归档清单、日志、实际 CMake source/link/install 输入及首次失败按字节哈希保存。
中文/空格路径搬迁验证通过，缺失和篡改实际 SDK 结果均被拒绝，不依赖生产日志绝对路径。

原 Pane 注释差异未修改；LuxEngine 排版差异未提交。Context 旧字段已按规范重写，用户原始字节和
binary patch 留在 P01 保护归档，保留字段的对齐意图映射为未提交差异。ProjectBuilder 补丁独立未应用。
没有修改 modules 公共头，本阶段不新增三前缀同步责任。
原生输入 NOT_RUN_USER_DEFERRED；Linux、IME、旧性能和历史免验不变。P06 的 sanitizer 责任仍保留。
不修改 main、历史快照，不合并或发布。

归档共 938 份文件、52 条实际命令；manifest SHA256：ba2f2fefb299687d63cea05c00b8f777347cfe231c9b73e9f3fbd5b958a4b8cc。
