# MA06 依赖准备：共享图事务验证

实现：`617403987b91940b4851b2d8007755a96e492494`。lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
本批提取规范 MA08-A/B 的实际共享结构事务，供尚未完成的领域图迁移使用。
**MA06 和 MA08 均未完成；本次完整验证为 PARTIAL。** 原 SDK public_host 在最小化 phase 8 失败，
未重跑覆盖或改判。FlowNodeCatalog、真实编译扩展接点、旧节点结构权威删除及完整 MA08 门禁仍待完成。

## 责任与改变

现有 graph target 提供唯一 `GraphEdit` 候选/提交算法。MaterialGraphEdit、FlowGraphEdit 删除原
拓扑/布局候选与对应状态位，组合共享实现。语义 payload、反射/变量/导出校验和移除对象的保留仍属于
领域编辑。Material 的原 pin 注册算法只有一份，直接图插入与事务通过窄 concept 共用。
没有新增库、Session counter、历史、队列或管理器。

规则明确为：已发出的 NodeId/PinId 不回收。放弃候选或后续校验失败只保留高水位，不发布节点、pin、
链接、布局或 payload。耗尽哨兵不回退；显式恢复允许原 ID。准备惰性复制，纯布局变化不复制拓扑，
空编辑不复制两份结构。提交只交换纯值与已准备的领域存储，不调用用户代码；移除 payload 仍在提交后
由原事务/日志 owner 接管或释放。

统一位置校验：未知节点返回 UNKNOWN_NODE，非有限位置返回 INVALID_SEMANTIC；unplace 对存在但
无布局的节点幂等，对不存在节点拒绝。此前 Material 的 unknown unplace 静默成功和 Flow 的笼统
INVALID_ID 不再保留为两套规则。GraphEdit 是同步独占借用，查询视图须在下一次修改后重新取得。
尚未删除原 Node/Pin 自带身份，也未宣称 PinId payload 查找已达 O(1)。

## 修前与修后

真实安装 SDK `dd9fd727a52c482b565fc0bec810ef3e1cbc8b6c`：Material 放弃候选 node=2/pin=2 后
下一次仍为 2/2；Flow 放弃 node=2/max pin=4 后下一次仍为 node=2 且 pin 未推进。两个退出42结果、
完整源记录与 owner 不变断言保留；冻结夹具另用同一旧 SDK 构建执行，记录源码和可执行文件哈希。
不是声明探针或模拟实现。修后两个身份用例通过。

真实领域测试还覆盖 Material 同 ID 删除/替换/恢复、payload 值和未修改节点地址、Flow 动态 pin 的
删除/保留 owner/恢复、所有 pin/链接/布局记录、编码/解码往返及两域纯布局局部性。这里验证的是图
事务的恢复能力，不冒充新 Editor History 或完整 Graph UI 集成。

Windows/MSVC RelWithDebInfo，固定 clean tracked 独立源码；复用构建树的增量 all，不是冷构建：

| 范围 | 实际结果 |
|---|---|
| tracked snapshot、Editor all/no-work/CTest | 通过，141/141 |
| PLAYER all/no-work/CTest | 通过，77/77 |
| 全新 SDK 原消费者 all/no-work | 通过 |
| 全新 SDK 原消费者 CTest | **14/15；public_host phase 8 FAIL** |
| 图模块安装消费者 | 3/3，加 GraphEdit 独立公共头 |
| 实际 Material/Flow 编译安装消费者 | 4/4 |
| 实际 source/include/link 闭包 | 686/629/57/3/4 个编译单元；graph 不依赖两域，三模块不依赖 Engine/Editor/UI |
| 三个 modules 公共头 | 新 SDK 及 Debug/RelWithDebInfo/Android include 三前缀逐字节一致 |

实际编译对照在同一新 SDK 比较直接构图与共享事务构图：Material 两份 SPIR-V 为1431/12860 words，
逐字节相同；Flow AOT object 为894 bytes，逐字节相同。这不是新 Flow 扩展节点编译、旧 SDK 编译器
二进制对照或整个编译器迁移资格。原测试名称没有删除，仅增加 graph.edit/material_edit/flow_edit；
原 SDK 全部15项仍执行，失败如实保留。

当前失败输出：`Render request 1174405120; backend status present 1, value 3`。
原 `Q-LR03-HOST-MINIMIZE` 的 phase 8 失败日志按原 SHA/哈希复核并附入归档。两次都发生在最小化阶段，
**尚未证明根因相同**；本批不改 Renderer，也不以开发树/source 测试通过消除此问题。

## 证据与范围

外部归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/graph-evidence`。
1382 文件、36 条实际命令；manifest SHA256：`cf67fccc2bd9f93d6f941348ff58ebf6c080df6bc0bbb782d06a29fa60ebc7ac`。
包含成功测试原始输出、首次失败、旧 SDK 负例和实际闭包输入；中文/空格路径搬迁、缺失与篡改 SDK
日志拒绝检查通过。仓库只提交可重复测试、契约与本精简记录。

六处用户差异哈希保持；ProjectBuilder 外部补丁未应用，main 未改。LR08 PARTIAL、Linux 未测/未通过、
原生输入延期、IME 未测及历史 WAR 状态保留。没有本轮 sanitizer/Linux/人工输入资格；Android 仅同步头。
