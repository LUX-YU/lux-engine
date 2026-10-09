# Mechanism / Authority MA02：错误归属与稳定身份

实现 `46faa2e35ac570baf296fa1c4cd6ae39388dad46`，分支 `codex/editor-framework-v2`。
lux-cxx 源码与安装依赖为 `0a0e7419fc7229df6e372cd35a540249f92250ef`。
本阶段 Windows 范围通过；LR08 PARTIAL、Linux 未通过与既有延期保留。

## 实际变化

Process 自己提供 ExecutionError 的描述登记和 `toError(EExecutionError)`，删除 Editor 的对应描述与转换体。
登记仍在装配边界执行；失败路径只查常量 ID 并填充数值参数，不注册、不分配、不取得 Registry 锁。
12 个原 canonical name、ID、消息、恢复分类与参数契约保持不变；全部 256 个底层枚举值均有测试。
Process 公开头不引入 Editor，core error 不反向依赖 Process。

SemanticType 删除手写 FNV 算法，复用 lux-cxx 的 canonical fnv1a；保留零值保留规则。
真实旧 SDK 与本次新 SDK 分别编译运行同一夹具：14 个现有 semantic 类型 ID、12 个执行错误输出逐字节一致。
这证明所列现有协议身份保持，没有扩张为任意 Unicode 名称兼容性声明。

## 固定实现验证

独立干净源码 `D:/LuxQualification/ma-source`，ValidateTrackedSnapshot 通过。
Editor/PLAYER 复用 MA01 的构建目录进行增量全量目标构建，本轮不称为新的冷构建。
所有构建使用 `all -j 4 -- -k 0`，第二轮均 no work：

| 配置 | 实际结果 |
| --- | --- |
| Editor RelWithDebInfo | 121/121 |
| PLAYER RelWithDebInfo | 58/58 |
| 全新 SDK 原消费者 | 15/15 |
| 纯 Process / Semantic 安装消费者 | 3/3，另有公共头独立编译 |
| 原 / 新 SDK 协议输出 | 14 项身份、12 项错误逐字节一致 |

逐测试名称比较，源码与 PLAYER 只增加 semantic.identity、process.execution_errors、
process.execution_errors_collision；原测试未删，原 SDK 15 项名称不变。
实际编译命令、Ninja 链接和 File API 依赖核验：纯 Process 消费者不链接 Editor；
Error 不依赖 engine；新 SDK 不依赖源码私有头、构建 DLL 或 legacy。
Semantic 公共头同步三个规定前缀，Android 仅同步。六处用户差异哈希不变。

## 证据与未测范围

新前缀 `D:/LuxQualification/ma02-install`。
外部证据 `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma02/evidence/`，
1324 文件、39 次实际命令，含修前/修后安装夹具、日志、原始 File API/链接输入、实现差异及核验脚本。
manifest SHA256：`01eea312c612a2367d47701657bd328dfbd5ec72607564da6e10bf95db4a094c`。

Linux 未通过、原生输入 USER_DEFERRED、IME 未测、Q-LR03-HOST-MINIMIZE OPEN 与历史 skinned WAR 保留。
本次未重跑 sanitizer 或旧性能长测；不改写 LR08 原资格。原 ProjectBuilder 补丁仍未应用。
MA03–MA11 继续按规范实施，本记录不宣告整体完成。
