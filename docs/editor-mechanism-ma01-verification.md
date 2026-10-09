# Mechanism / Authority MA01：删除零消费者机制

实现 `8e8f99f9ea8cbe4700f9ff4ffcefceaceb2db076`，分支 `codex/editor-framework-v2`。
基线 MA00 `79c227d3a`；lux-cxx 源码与安装 SDK 均为 `0a0e7419fc7229df6e372cd35a540249f92250ef`。
本阶段 Windows 范围通过。LR08 PARTIAL、Linux 未通过及既有问题按用户授权保留，不因此宣告全平台通过。

## 实际变更

按 MA00 的实际产品消费者为零结论，删除 core services 与 events 的完整提供者、组件与安装入口，
以及其专属插件测试、`service_tasks.cpp`、安装消费者和两个无人使用的旧测试适配。
总计 23 个文件变更，其中删除 17 个文件。没有迁建另一套服务容器或将事件消费者改接 Object。
EditorServices、EditorComposition、项目服务的惰性构造、唯一 owner、递归拒绝与逆序释放均保持原实现。

Log 只修正错误的 DomainEvents/event-bus 说明，仍使用原输出实现；输出寿命属于 MA04。
质量规范明确活动 Editor 不引入旧 ServiceScope/Resolver。CMake 同步整理相邻调用和多行括号。

## 固定实现验证

独立干净检出 `D:/LuxQualification/ma-source`，先通过 ValidateTrackedSnapshot，再运行全量 all -j 4 -- -k 0。
所有以下结果均绑定该实现 SHA，而非工作区后续改动：

| 配置 | 全量构建 | 第二轮 | 实际测试 |
| --- | --- | --- | --- |
| Editor RelWithDebInfo | 1238 步通过 | no work | 118/118；含 40 项 GPU、8 项 desktop 标签 |
| PLAYER RelWithDebInfo | 1141 步通过 | no work | 55/55 |
| 全新 SDK 消费者 | 185 步通过 | no work | 15/15；含公开头与真实 DLL/UI 消费 |

与 LR08 固定配置逐测试名称比较，源码仅删除 `services.core`、`services.plugin`、`services.tasks`，
SDK 仅删除 `sdk.services`；其它测试名称与原断言保留。删除的是已删除子系统的测试，不以数量相等证明行为保留。

原 SDK 上，同一真实 find_package 夹具可使用 services/events；新前缀
`D:/LuxQualification/ma01-install` 上二者明确以 `Component ... is not available` 拒绝。
同夹具 object 正例通过，排除工具链或依赖损坏导致的假负例。

实际 File API、compile_commands 与 Ninja 链接检查：Editor 370 targets/655 TU，PLAYER 325/599，SDK 56/57。
无删除模块的源码、include、target 或安装组件；SDK 无源码私有头、构建 DLL 或旧 Editor 补全依赖。
PLAYER 没有 Editor 源码。模块公开 Log 头已同步 Debug、RelWithDebInfo、Android 三前缀，
精确废弃的 services/events 头已清理；Android 仅同步，不记作构建验证。历史 SDK 保留原样供前后比较。

## 证据与范围

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma01/evidence/`。
32 个文件保存实际命令、原始输出、实现 diff、删除清单、测试名称对应与闭包核验脚本；
manifest SHA256：`aaad08dd490ce87c73a97a758d73ae55f6819f338d887888b6c1599292c9af6e`。
唯一可变施工记录仍位于 `.internal/editor-redesign/terminal-architecture/`。

六处用户工作区差异逐字节哈希未变，原 ProjectBuilder 补丁仍独立保存、未应用；main 与历史快照未改。
Linux 未通过、原生输入 USER_DEFERRED、IME 未测、Q-LR03-HOST-MINIMIZE OPEN 与历史 skinned WAR 保留。
本阶段没有重跑 ASan/UBSan 或旧性能长测；未修改路径引用 LR08 固定 SHA 的原范围，不扩张资格。
MA02–MA11 尚未验收完成，按已授权计划继续推进。
