# 05　AGENTS.md 遵守情况与执行清单

## 1. 规范来源与本次核验范围

用户上传文件为 313 个实际文本行、CRLF。原字节 SHA-256：

`4ac02b300d99fd05ff7bcb5bced4bfc6f47b8e30caaa1a8821ace240e8f75357`

将 CRLF 仅在比对中规范化为 LF 后，Git blob 为 `1fd3123156dc7f59f98cbcd422f227a16352cddc`，与固定提交中 GitHub 返回的 AGENTS blob 一致。附件仍按原字节保存，不改写用户规范。

本次完整读取了用户提供的规范；代码只进行了记录在来源索引中的定向核查。没有完整源码 AST／全仓格式检查、独立引擎构建、生成器和安装运行，因此不能回答“所有条目均已遵守”。

## 2. 对上轮评价的校正

AGENTS 明确要求：复杂条件先分解为具名 `const bool` 语义组，最后判断聚合结果；外提不能破坏短路安全。

因此，“用了多个 is_* 布尔变量”本身不是违反可读性规范。不能为了缩短代码，把它们统一合并回长串比较。正确整改是：组名准确、独立校验分组清楚、无意义的同义中转减少，最终仍符合用户要求。

历史是否应是数据／执行者分离，是本次新增设计要求；AGENTS 并没有写“所有操作只能是成员函数”或“所有数据都不能有成员”。不能伪称该规范早已规定这项选择。

## 3. 当前能够直接指出的例子

| 条目 | 源码观察 | 判定 | 本轮处理 |
|---|---|---|---|
| 类型命名 | `Skeleton.hpp` 仍定义 `Bone_t`，注释承认历史概念重名。[S41] | 不符合当前 PascalCase 基线；这是存量规则问题，不是插件工厂故障。 | 改到该数据定义时准确正名并迁移实际消费者；不得保留同义 alias。 |
| 复杂判断分组 | DetachedView 构造中直接用 `!code.valid() || !pane || pane->attachedRoot() || pane->parent()`；EditorViews 容量条件也将不同校验直接组合。[S14,S06] | 与 AGENTS 的具名分组要求不一致；短路当前有保护，不能重排到先解引用空指针。 | 先 code／pane 存在，再有效时计算 attached／parent；聚合具名结果。 |
| 长返回类型／调用签名 | SceneConfigurationInputs 的嵌套 std::function、ViewStateResult 的长返回声明等。[S22,S14] | 存在应按 120 列及语义别名规则整理的实例；本次不报全仓行数。 | 用对应结果／回调的有意义别名，函数名另行；不把模板标识随意从 `<` 中间拆开。 |
| 只需描述却包含具体 View | Contributions.hpp 通过 InspectorView.hpp 得到 InspectorComponent。[S11,S19] | 公共依赖归属不够精简，亦违背 QR18 方向；并非简单删 include 就能编译。 | 迁出真实轻量描述定义，View 和贡献共同使用；禁止转发 shim。 |
| 可变快照破坏不变量 | ProjectCatalogSnapshot 的 public owner-dependent views。[S16] | 明确类型设计问题，主要对应 QR01/09/15；不是命名格式问题。 | 私有拥有数据 + 只读观察／索引。 |
| 动作和载荷分离 | ResultIntent / WorkspaceIntent。[S09] | 语义设计问题，对应 QR01/04；不能仅运行格式器修复。 | 有限请求 variant。 |
| 一般不使用异常 | InspectorView 的构造型 component 路径包含 catch bad_alloc 后 terminate，其他 catch 转 CODEC。[S18] | 与“允许边界捕获后立即转 Lux error”的文字存在需要裁定的分配失败政策差异；不能直接宣称全面合规。 | 在可恢复的 prepare/factory 边界返回准确错误；已进入无失败 commit 的 fatal 分配政策另行明确，不能假造回滚。不要全仓机械替换 terminate。 |

上表最后一项是**规范／既有失败政策的一致性问题**，不是已复现的功能 bug，也不是要求禁用 C++ 异常处理机制。

## 4. 逐类规范矩阵

| AGENTS 主题 | 当前依据／边界 | 实施方必须产生的结果 |
|---|---|---|
| editor/engine/modules 与五层 | 根结构和已读 provider 符合目标；完整链接未独立重跑。 | 真实 include/target/生成器/静态闭包；新增插件路径不反向依赖。 |
| 命名：类型、V alias、E enum、成员 | 代表性头多数符合；未做全仓 AST。 | 修改闭包内检查每个新/改类型；V 前缀仅对 variant alias，不误改 expected alias。 |
| 120 列、4 空格、括号 | 有直接待整理实例；未跑全仓 formatter。 | 按实际语义格式化改动文件，样例先人工核验，不生成全仓无关排版 diff。 |
| 复杂判断与短路 | 分组有正确正例，也有直接串联反例。 | 检查解引用、下标、整数运算的前提；阶段布尔量必须真的保护访问。 |
| include/sinclude/pinclude/src | 当前职责源已分层；精确安装依赖需构建核实。 | 同目录多 target 用实际 provider；不能对插件导出项目私有头。 |
| 不留兼容别名与自指 using | 已有阶段报告表明多处清除，不代表全仓为零。 | 新增替换 API 的全部调用和安装同步；历史参考不算活动 shim。 |
| 组件头不依赖行为层 | 本次没有遍历全组件头，不能认证。 | 对改到的组件头和它们生成依赖逐项查；公共编码约定归原 resource description。 |
| 公共头不拖重依赖 | 上述 Contribution/Inspector 是需要处理的依赖关系。 | compile/header closure 验证；需要完整 value/基类定义的不强行前置声明。 |
| 不主动 throw、热路径无 try/catch | 本次未作完整热路径分类；不能仅搜索关键字下结论。 | 区分业务 throw、STL/第三方边界 containment、测试代码；实际 dispatch/drain 中的捕获不能靠重命名 helper 隐藏。 |
| 错误与日志出口 | 原 structured errors/Result 继续复用。 | 不把错误一律转 BUSY，不让 render 链接 log，不新增 fprintf/cerr 宿主出口副本。 |
| assert 不承担 release 验证 | 本次未全仓检索；原 History 使用真实错误和 fatal 条件。 | 外部非法输入在 RelWithDebInfo 下真正拒绝；断言只用于内部已证明条件，不能代替运行返回。 |
| ECS observer 只记延迟命令 | 当前整改路径需特别保护增量投影。 | 观察中不直接改 Registry；排空期间新工作留后批，销毁读句柄后排队。 |
| observer 连接折入存量 | 未独立验证所有系统。 | 新改 observer 同时处理先建对象后连接；不能依赖构造顺序。 |
| on_update 用 patch/replace | 字段／增量投影修改时必须检查。 | 不用 get<T>().field=... 绕过通知；按原 schema/operation 接口执行。 |
| 异步就绪保留原轮询 | Process／GPU 的完成不是一次结构 signal。 | 不为统一 observer 而删除轮询；完成与资源装回世界在原安全点。 |
| all/-j4/-k0、两轮构建 | 历史日志不等于本轮已跑。 | 最终改动闭包完成后 all；CMake 改动第二轮 no work；构建与实机验证串行。 |
| modules 公共头三前缀同步 | 本轮若未改则不触发。 | 改到 modules 公共头时同步 Debug/RelWithDebInfo/Android include 并比对；Android 构建仍非默认任务。 |
| tracked clean 资格 | 容器无法获取完整 clone，不冒称执行。 | 实施方先 ValidateTrackedSnapshot，再固定 clean commit；不靠忽略文件补齐源码。 |
| QR01–QR22 和唯一账本 | 本包按原责任和记录位置。 | 只在原账本扩展 EC1；旧验收的结果和 SHA 不改写。 |

## 5. 异常规则的执行方式

不得主动用 throw 表达 Lux 的域错误。公开 runtime/domain 默认 noexcept 与 expected；`noexcept` 不自动说明“不分配”或“所有失败都可恢复”。

边界捕获必须有实际理由：Builder、Codec、fallible factory、toolchain 或外部插件调用。捕获后立即转换，不把异常传播跨 DLL、Task、Script、System 边界。

本轮不全局打开 `-fno-exceptions`／`/EHs-`。不向每层加入相同的 try/catch。清楚区分：

- 准备期正常可报告错误：返回结构化 Lux failure。
- 第三方异常：在最近的允许边界转换并保活错误载荷代码。
- 已声明为无普通失败的提交阶段：先通过准备消除可恢复失败；无法恢复的契约破坏按既有 fatal 政策处理，不返回“失败但已经改了一半”。

## 6. 机械检查和语义检查分开

可机械辅助：列宽／缩进、enum/variant 命名候选、私有头出现在安装列表、已删除 API 引用、throw/try/catch 候选、未经分类的调用依赖。

必须语义复核：短路安全、catch 是合法外部边界还是热路径补偿、某个 include 是否只为完整值定义、一个 bool 是否为动作状态、一个公共快照是否能被误构造。

不提供“扫描到 0 条字符串 = 完全遵守”的资格。误报和不适用项须在同一账本说明，不能简单关闭整个规则。
