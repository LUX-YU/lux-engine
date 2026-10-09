# MA06 首个闭包：Context 的非拥有扩展访问

实现 `14657276936814d2d757ad38a35192ff599b9248`；lux-cxx 保持
`0a0e7419fc7229df6e372cd35a540249f92250ef`。本记录仅覆盖 Context 接入，**MA06 整体尚未完成**。
Flow/Material 节点目录及其与 MA08 payload/编译契约的衔接仍待实施，不以类型化查询表替代这些工作。

## 已完成的职责

EngineContext 和 EditorContext 保存同一 ContextExtensions 值类型。
装配值只登记已存在对象的 TypeToken/地址；Context 构造时一次发布。
运行表不可复制、移动或赋值，没有注册、删除、替换、工厂、依赖解析或惰性实例功能。
查找直接借用原领域对象，缺失返回 nullptr；const Context 只返回 const 扩展面。
对象、类型名称所在代码及描述的寿命仍由领域 owner 承担，须覆盖 Context，不由表保活。
EditorServices 的原工厂、唯一实例及释放顺序保持不变。

首次全量构建暴露 Editor 公共头缺少 engine_context 的公开 provider；已将真实 target 依赖改为 PUBLIC，
没有给测试添加源码 include 路径，也没有为一张表增加服务库。失败日志保留。

## 固定实现验证

独立 clean tracked 源码 `D:/LuxQualification/ma-source`，ValidateTrackedSnapshot 通过。
Editor/PLAYER 复用构建树执行全量 all -j 4 -- -k 0，第二轮无工作；不称冷构建。

| 配置 | 实际结果 |
| --- | --- |
| Editor | 129/129，含既有实际 GPU/desktop 回归 |
| PLAYER | 66/66 |
| 全新 SDK 原消费者 | 15/15 |
| 纯 EngineContext 安装消费者 | 1/1，另有 ContextExtensions 公共头 C++20 独立编译 |

新增测试使用实际 SimulationSystemRegistry 和真实 headless EngineContext，验证 10,000 次地址稳定查询、
重复拒绝、缺失不构造、const 查询、Context 不删除借用对象、独立 Context 不继承绑定。
原源码及安装 EditorContext 测试增加实际 AssetVfs 扩展绑定，保留原服务递归、失败重试、销毁顺序、
UI/工具及项目隔离断言。安装消费者仅使用安装头和库。

逐名称比较仅新增 engine.context_extensions；原 SDK 测试名称不变。
实际编译及链接闭包分别为 669/613/57/2 个编译单元，engine_context 不反向依赖 Editor、UI 或 PluginManager。
变更公开头与新 SDK 一致；本批没有修改 modules 公共头，不涉及额外 Android 头同步或构建。

## 证据与用户差异

新 SDK：`D:/LuxQualification/ma06-context-install`。
外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/context-evidence/`。
1320 文件、23 次实际命令，含首次构建失败、固定 SHA 输出、编译链接输入及保护证明。
manifest SHA256：`ba5614bb9817e453a16a207f80f5a7070ae033c47424ed4fd0d04c3140e36ad8`。

EditorContext.hpp 的同一功能插入分别应用于 tracked 原体和用户版本，只提交前者；原用户字节、补丁和映射后
哈希均保留，用户排版仍是未提交差异。其余五处用户文件逐字节不变，ProjectBuilder 补丁未应用。
main、历史快照和判定未改变。

LR08 PARTIAL、Linux 未通过、原生输入延期、IME 未测、Q-LR03-HOST-MINIMIZE OPEN、历史 skinned WAR
继续保留。本批没有新 sanitizer 资格，不将 Windows 通过扩大为整体完成。
