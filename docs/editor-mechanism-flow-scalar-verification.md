# MA06 依赖准备：Flow 算术编译语义

实现：`f3f1d4b4d0d4bb1fa5d9673317aee864443139e1`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
**本批算术提取通过；MA06、MA08 及整体任务未完成。**

## 实际改动

原 IR.cpp 中的整数符号、浮点分类及一元/二元算术指令选择迁入现有 Flow 模块，原算法体删除。
MLIR 后端消费已经选定的指令，不再自己决定有符号除法、余数、比较及有序浮点比较。
原按需输入求值、区域内缓存、类型转换、变量存储、控制流、Script Ability 和挂起分析保持原实现。

新增 ScalarLowering.hpp 位于项目级 sinclude，不安装。它仍接收旧 ENodeOperation，调用前仍须经过
原操作数校验/转换；不冒充最终扩展 SDK。内部指令不是序列化身份、第二张图或新的执行器。
没有新增 target、DLL、队列或 owner，也没有修改 modules 公共头，因此本批不涉及三前缀头同步。
安装消费者只使用正式公开 Compiler/Graph 头和安装库，不读取这个内部接线头。

FlowNodeCatalog、外部节点编译合同、原 Native reflection 注册的拥有语义以及两域图的最终 payload/
compiler/codec 接入仍待完成。此次提取不能代替这些工作，也不关闭 MA06/MA08。

## 实际验证

Windows/MSVC RelWithDebInfo，独立 clean tracked 源码绑定上述实现 SHA；复用构建树，属于增量验证，
不是冷构建。源 CMake 的原测试名称保留，仅 Editor 增加 flowforge.scalar_lowering。

| 范围 | 结果 |
|---|---|
| tracked snapshot、Editor all/no-work/CTest | 通过，142/142 |
| PLAYER all/no-work/CTest | 通过，77/77 |
| 全新 SDK 安装、实际 Flow 编译消费者 all/no-work/CTest | 通过，1/1 |
| 同一固定测试源码，旧 SDK `617403987...` all/no-work/CTest | 通过，1/1 |
| 旧/新 SDK 的实际 AOT 对象比较 | 123 个可观察状态读写事件，28,393 字节逐字节相同 |
| 实际依赖/源闭包 | Editor 688、PLAYER 630、旧/新安装消费者各 1 个 TU |

算术矩阵覆盖 8/16/32/64 位有符号和无符号整数、float/double 的 11 种二元运算和取负，以及三个
布尔操作。事件从实例状态读取输入并写回结果，不靠未使用的函数或被折叠的常量通过。
对象符号检查确认 123 个事件；反汇编原始输出已归档。
AOT 对象 SHA256：`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`。
这是实际编译输出等价，不声称运行了这 123 个事件，也不是外部注册节点资格。

保留两次夹具修正记录：第一版未被调用的函数被优化掉，只生成 878 字节，不能作为算术等价证据；
第二版把全部标量作为 Script ABI 参数，被原 SDK 准确拒绝。最终夹具使用原有实例状态和无参数导出，
没有扩大 Script ABI。最终相同源码分别由旧、新 SDK 独立构建执行。

File API 核验 Flow 模块实际传递依赖没有 Engine/Editor/UI；安装消费者未读取源码私有头或构建 DLL。
本批原 SDK 桌面消费者未重跑，其上一批 `617403987...` 的 14/15 和 phase 8 最小化失败原判定保留，
原失败日志按原哈希附入归档。当前源码测试通过不能消除此失败；同旧 Q-LR03 问题根因相同与否未证明。

## 证据与保留范围

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/scalar/evidence`。
1063 文件、33 条实际命令；manifest SHA256：
`f84ac11d0fd836fb374be707b171f47657dac567e3d530f0574510361850c636`。
包含实际成功输出、夹具失败、前后对象、符号/反汇编、CMake 闭包和继承失败记录；搬迁到中文/空格路径、
缺失与篡改真实 SDK 日志的拒绝检查通过。

六处用户差异哈希保持，ProjectBuilder 外部补丁未应用，main 未改。LR08 PARTIAL、Linux 未测/未通过、
原生输入延期、IME 未测、历史 WAR 均保持。本批没有新增 sanitizer、Linux 或人工输入资格。
