# MA06 — 现有 Flow Ability 描述寿命补正

本收据只覆盖现有 `ScriptAbilityNodeCatalog`、`ScriptAbilityNode` 及其真实消费者。
**MA06 尚未完成**：完整 FlowNodeCatalog/MaterialNodeCatalog、开放 payload 与编译合同仍待实施。
不以本次补正代替 MA08 的 GraphTopology 唯一结构权威迁移。

实现：`7fa115bfe830ca2504f2b9698b6f4c8b60366eb5`。
依赖 lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`，沿用上一收据的源码/SDK版本。
Windows/MSVC RelWithDebInfo；独立 clean tracked source、复用构建树的增量全量构建，**不是冷构建**。

## 实际问题与修正

旧安装 SDK（Context 实现 `14657276936814d2d757ad38a35192ff599b9248`）通过公开头和实际库运行两项负例：
目录准入后覆盖输入文本、节点构造后覆盖参数文本，均退出 42。输入当时仍存活，负例不依靠未定义的
悬空解引用取得失败。旧目录只复制 view，旧节点的参数/结果仍部分借用输入；两个原始输出已保存。

目录和节点现在各自拥有完整不可变描述，共用一份模块私有的存储构造算法。目录平面数组只是指向其
存储的查询视图；借用有效期明确限制到下一次成功准入或析构。拒绝批次不改变已有目录和借用。
贡献者的纯描述不包含执行回调，复制后无须保活其 DLL；这不代表将来的可执行节点 payload 可以不持代码责任。

源文件环境验证与目录准入共用原校验算法。前者调用无分配的 `validateScriptAbilityNodes()`，
删除仅为验证而构造临时目录的路径，避免深复制引入额外分配。节点的重复浅拷贝成员已删除；
死枚举 `ALLOCATION_FAILURE` 删除，普通堆 OOM 仍为 fatal。没有增加库、管理器或 PluginManager 依赖。

## 固定提交验证

| 范围 | 实际结果 |
|---|---|
| ValidateTrackedSnapshot | clean tracked SHA 通过 |
| Editor all `-j 4 -- -k 0`、第二轮 | 通过；第二轮 no work |
| Editor 完整 CTest | 132/132，原 129 项保留，新增三项 |
| PLAYER all、第二轮、完整 CTest | 通过、no work、69/69，原 66 项保留 |
| 全新 SDK、原消费者 all/第二轮/CTest | 通过、no work、15/15 |
| 新安装 Flow 消费者 | 3/3；两个公共头单独 C++20、无 RTTI 编译 |
| 实际 Flow AOT 编译 | 原全部断言保留，增加动态目录输入销毁后的实际编译 |
| 实际 DLL | 源码及新 SDK 均构建/装载/卸载 DLL，卸载后读目录、节点并再建节点成功 |
| 批次/增长 | 重复、同批次 schema 冲突、非法版本均拒绝；原目录保留；128 次动态名称准入后查询准确 |
| 安装与闭包 | 672/616/57/5 个实际编译单元核对；Flow 传递依赖无 Engine/Editor/UI；私有存储头不安装 |
| 三安装 include 前缀 | 两个修改的 modules 公共头逐字节一致；Android 只同步，不是构建验证 |

全部资格命令绑定上述实现 SHA。开发阶段的第一轮 132 项及末次 15 项定向验证另存，不替代固定 SHA 结果。
SDK 测试不读取生产源码的 include/pinclude/sinclude，不链接开发构建 DLL。目录/节点元信息纯复制测试
与实际编译、DLL 卸载测试分别登记，没有把前者称为后者。

## 证据与保留范围

归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/ability-evidence`。
1,331 个文件、28 条实际命令；manifest SHA256：
`8e4131c46c0052f10bef5b6a596f8948a7833c871cf35d6447b08975b635b96b`。
搬迁到含中文/空格路径后完整校验通过；删除或篡改实际安装消费者测试输出会被拒绝。
校验只依赖归档相对路径及其字节，不依赖原生产机器日志位置。

六处用户工作区差异的哈希均与已保护版本一致；EditorContext 使用上一收据记录的映射后版本。
ProjectBuilder 外部补丁未应用，main、历史判定及历史证据未改动。

LR08 整体 PARTIAL、Linux 未执行/未通过、原生输入延期、IME 未测、既有 minimize/WAR 问题保持原记录。
本次没有新增 sanitizer 或跨平台成绩；不将旧 SHA 的成绩改写为本次执行。
