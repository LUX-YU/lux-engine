# MA06 — Flow 语义载荷与领域错误验证

实现：`dd9fd727a52c482b565fc0bec810ef3e1cbc8b6c`。lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
本收据只覆盖 FlowNodePayload 和 FlowForgeFailure 的模块提供者。
**MA06 未完成**：FlowNodeCatalog 及真实编译接点仍待接通；MA08 的图结构、内置节点、codec
与编译分派迁移也未完成。载荷测试不冒充开放 Flow 图编译。

## 实际改变

FlowNodePayload 唯一拥有节点语义对象，无 NodeId/PinId、图成员指针或链接。移动、fallible clone
和析构复用原 CodeLease，代码保活覆盖插件析构及返回；不创建另一套图或执行器。
注册/目录尚未完成，不发布空 compile hook 或旧 Node creator 的同义外壳。

FlowForgeFailure 的唯一声明迁入现有 flowforge 模块，Toolchain 直接消费；原 Compiler.hpp
继续提供真正的编译函数，不是转发头。原拥有型消息、节点/引脚诊断保留，24 个
有效错误枚举数值与旧 Git 对象逐项相同。删除无生产者的 ALLOCATION_FAILURE；普通堆 OOM fatal。
MLIR/LLVM 降低、AOT、反射及 ScriptAbility 执行算法未改。

## 验证

Windows/MSVC RelWithDebInfo，固定 clean tracked 源码、复用构建树的增量 all；不是冷构建。

| 范围 | 实际结果 |
|---|---|
| ValidateTrackedSnapshot | 固定实现 SHA 通过 |
| Editor all / 第二轮 / 完整 CTest | 通过 / no work / 138/138 |
| PLAYER all / 第二轮 / 完整 CTest | 通过 / no work / 74/74 |
| 全新 SDK 原消费者 | all / no work / 15/15 |
| 仅 Flow 模块 SDK | 2/2，两个独立公共头、真实 DLL，无 Toolchain |
| 另启实际 Flow 编译器的 SDK | 3/3，保留原 Ability 编译测试全部断言 |
| 闭包 | 683/626/57/5/6 个编译单元；Flow 传递依赖无 Engine/Editor/UI |
| 公共头 | 两个新 modules 头与三个 install include 前缀逐字节一致；新 Compiler.hpp 与新 SDK 一致 |

Editor/PLAYER 原测试各增加 flow.node_payload 与 flow.node_payload_plugin；原名称无删除，原 SDK
测试未缩减。独立 SDK 只读取安装公开头和库，不读取源码私有头或 build DLL。

实际载荷覆盖 RuntimeObject 字符串深复制、独立修改、错误传播、空 clone 拒绝、无效代码租约
在构造前拒绝、自移动、移动赋值和最后代码 owner 的清理次序。DLL 测试先释放加载方引用，
再执行插件 clone、保留原值、析构最后载荷，最后卸载库；拥有型失败文本可在卸载后读取。
编译器测试实际生成 AOT object，验证冒用节点类型、缺失 Ability、schema 不一致以及拥有目录
元信息的编译路径。此处没有声明新载荷已被旧图消费，也没有声称二进制产物逐字节对照。

## 证据与保留范围

树外证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/flow-payload-evidence`。
1358 文件、25 条命令；manifest SHA256：`febe268f26ea299734fc887c38c4452aad1b9aa89af2955b9024d46ab0f2c9ea`。
原错误定义取自 `5ffb2489d489720d90b503b369f4797568cf665a`，这是源码合同核对，不冒充旧 SDK
运行。归档搬迁到中文/空格路径通过；缺失或篡改实际 SDK 日志必须失败。

六处用户差异哈希保持，ProjectBuilder 外部补丁未应用，main 未修改。
LR08 PARTIAL、Linux 未执行/未通过、原生输入延期、IME 未测和原 minimize/WAR 判定继续保留。
没有新增 sanitizer、人工输入或 Linux 成绩；Android 仅同步头。
