# 06　EC1 实施批次、删改清单与验收

## 1. 总执行规则

本轮只有一个 EC1 阶段，内部 S0–S7 按依赖推进；每批完成可构建的责任闭包，不能积累多个未完成实现再用兼容层粘接。

原 P13 已结束的事实保持。不得从旧数字推断“本轮必须至少多少项测试”。测试按真实行为映射，不能删除原断言只保名称，也不能为了数量固定保留已淘汰 API。

S0 必须核对当前实际 HEAD。若代码已经合入 main，使用授权开发位置和现行提交，不回退、不重建同名分支；本包本身不授权提交代码或删分支。

## S0：固定来源、事实和真正扩展范围

### 输入

当前源码、上传 AGENTS、docs/editor-quality、已有 P10Q 性能记录、最近 P12/P13 交付范围与用户补丁状态。

### 工作

- 记录 HEAD、分支／worktree、工具链、真实 lux-cxx/imgui/node-editor/toolset 安装版本；不用本包旧依赖 SHA 覆盖较新已验证依赖。
- 从真实 Git 文件与调用符号生成 EC1 处置表：路径、符号、实际 target、public/sinclude/pinclude、消费者、动作和替代位置。
- 将 O01–O09、D01–D09、P01–P06 与 AGENTS 表逐项复核；本次未知项保持 UNKNOWN，不能通过照抄变成已证实。
- 列出开放轴：作者资产／会话／内容视图／可选导入与派生产物／场景适用性。固定阶段枚举、wire 标签和旧数据映射不列入开放整改。
- 核对已有 Skeleton 数据与 codec、参考骨骼编辑动作；不创建重复的 asset 或序列化规则。
- 保存旧工厂、反射、History、保存、Run、GPU、输入及用户免验的行为映射。

### 出口

所有计划改动都有实际 provider；没有“添加一个接口以后再找消费者”的空项目。记录缺失资产 registry 等实际差距，不发明它已经存在。

## S1：数据／执行语义和类型不变量

### 必做

1. 按第 03 文档迁出 EditHistory 的执行算法，新增最小 EditExecutor，原日志保持唯一。
2. 更新 PreparedEdit 权限、各 Session、运行暂停编辑、历史测试及公共安装消费者。
3. 将 PreparedSessionData 的可执行准备语义正名并更新所有生产调用；无转发 alias。
4. ProjectCatalogSnapshot 私有化 owner-dependent 观察，加同快照查询。
5. ResultIntent、WorkspaceIntent 与 Inspector 布尔动作按真实合法状态收敛。

### 明确删除

EditHistory 的旧变异成员声明与定义；旧调用；PreparedSessionData 的同义兼容名；快照公开可写视图；action 与不匹配 target 的手工配对；不再使用的错误分支。

### 必须验证

原所有 History 行为，包括 prepare 失败、NO_CHANGE、预算、裁剪、旧 redo、两 executor 重入、publish/清理回调、code pin、关闭通知和跨 DSO 身份唯一性。测试必须真的经过新执行者，不能保留一份旧算法作运行 oracle。

在 freeze/description 类改动时不为了“纯数据”解除 RAII、线程或借用限制。

## S2：正式基础能力与插件激活

### 必做

- 给外部插件提供可独立使用的资产目录／冻结读取、会话、历史执行、Pane 构造、Host 请求、命令、Process、发布等正式能力。
- 每项优先复用原 public provider；不足时在原 owner 补窄接口或提取真实职责。
- 建立现有扩展装配中的显式能力注入、activation 生命周期和依赖版本协商；不能把 ApplicationImpl 指针伪装成服务。
- 让一个内置工具先迁到这些能力，证明不是“外部 SDK 另一套包装”。
- 将公共描述从具体 View 头中剥离，依赖按真实层放置。

### 出口

可构建三种安装消费者：只读资产的无 UI 工具；只创建／登记 Pane 的独立窗口；无 UI 的真实会话编辑与保存。它们不需要链接整个 EditorApplication，不借源码私有头。

插件仅需 CPU 能力时不被强制要求 renderer／LLVM；确实请求 GPU／编译时显式链接。

## S3：开放内容路由与完整内置迁移

### 必做

- 采用第 02 文档中的稳定来源类型、格式与会话／视图关系。
- 演进 Manifest 以容纳开放类型；v1 读取、未知类型保留、新格式显式发布和中断安全一起实现。
- 删除 Application 中打开、保存、后缀、视图组合和恢复的三类硬编码。
- 内置 Scene/Material/Flow 也通过同样的关系和能力激活；不保留快速特例作为业务后门。
- 派生产物发布移出闭合 VCompiledSource 产品表，复用领域 compiler 和公共发布边界。
- 内容—视图关联支持零／一／多内容引用，并保留固定目标、关闭与恢复规则。

### 遇到缺 provider 的行为

缺格式 provider：保留目录记录与原字节，报告不可打开。
缺视图 provider：内容可以保持已发布且可查询，显示失败不得回滚合法内容。
缺保存角色：明确 UNAVAILABLE，不假装 clean。
恢复找不到 exact ViewType：保留记录，不随意改用同种内容第一个窗口。

### 出口

所有三类内置内容通过通用路径完成打开、Save As、重载、SaveAll、关闭和恢复；源码中没有要为第四类内容再修改的重复路由表。

不要求与开放性无关的旧格式标签 switch 消失。

## S4：统一适用性

### 必做

- 从原描述形成版本化事实观察；组件、对象创建、Feature 和工具共用需要的纯判定。
- 区分不适用、暂不可用、无效目标／读取失败。
- 内置规则与插件规则相同接入；各领域仍拥有自己的合法性实现。
- 删除 Inspector 的无条件全 schema 候选和 Outliner 只按全局 Transform 注册启用的旁路。
- 保留未知已存数据；场景能力不足不等于可以删除 payload。

### 出口

2D／3D／混合／无层级／已安装未声明／已移除插件／不同分区契约均有行为测试。改变一个事实源后，各相关消费者得到一致可解释结论；不要求统一 UI 文案。

## S5：针对性性能与检查收敛

### 默认必做

- Outliner 的稳定身份折叠状态和 O(C×N) 查询整改。
- 同一 ProjectCatalogSnapshot 的 by-id 索引，ModelCreation 依赖核验不逐 primitive 扫全目录。
- 删掉已被构造契约证明且中间没有失效边界的重复 lookup/describe；每处附有效期说明。
- 新路由按注册／项目 revision 预建不可变查询，不在每帧调用全部插件工厂或遍历全项目。

### 测量后裁定

- SceneSource 的 N×S 扫描：以代表性实际 schema／组件分布观察；能用现有池／成员关系改为实际存在组件遍历时整改。不能在没有修改收益时新建同步困难的全局索引。
- SceneProjection：记录至少“无变化、普通字段、结构、配置”的成本。优先避免与显示无关的更新；字段增量仅在原组件 API、observer 和退休协议可保持时采用。完整结构／未知变更仍有准确 fallback。
- copyOpaque／freeze：只共享真实不可变拥有块，不把调用者可改的 shared_ptr<const T> 当冻结保证。

**裁定不意味着可以不做任何调查。** 必须给出对应真实调用、规模和算法／分配计数；若保留原实现，说明成本与理由，不写“已优化”。

### 计时限制

不补旧 50k 慢算法样本；不固定要求每项 100 次。用短代表性样本、操作计数、必要的分配和 inclusive/exclusive 观察回答问题。构建与实机采样不并行，计时和插桩计数分开。

“栈浅”不是出口；在优化构建里观察真正间接调用、重复数据处理和耗时，而不是统计源码方法数。

## S6：外部骨骼编辑器验证

### 骨骼类型不是新造的

复用 `lux::rdesc::Skeleton`、`SkeletonAsset`、其 AssetTypeId 和 TAssetSerDeser。[S35,S41] 作者 Session 可以是插件的新领域工作副本；运行 Skeleton 数据和 codec 不复制。

最小真实能力：读取一个小型合法骨骼、显示已有骨骼条目、修改名称及至少一个受控且可验证的已有属性（如 global_transform）、Undo/Redo、冻结、保存与重开。不要用只有一颗整数的 FakeSession 冒充骨骼编辑器。

骨骼父顺序与 mesh 索引引用必须正确。若展示 reparent／重新排序，需要保证依赖引用和 bind/inverse-bind 约定；本轮不以此为必选功能，禁止为完成验收偷偷破坏下游 mesh。

### 同一套 API 必须通过的三条路线

| 路线 | 验证 |
|---|---|
| 低层无 UI | 插件只通过安装 SDK 查询／读取资产，创建会话，执行历史，保存和重读。 |
| 自由 Pane | 插件直接组合原 Pane/Element，登记、挂载、描述、焦点、关闭；不要求成为某个固定 IEditor。 |
| 完整内容工作流 | 项目登记、双击打开、两个视图共享同一会话、保存／Save As／重载／关闭／恢复；宿主不含 Skeleton 特判。 |

可以用同一个插件和同一个测试程序的不同模式验证，不必建立三个 SDK 系统。

### 强负例

- 注册了新类型却忘记视图：内容留存，显示失败准确。
- 同一格式两个默认编辑器：返回歧义，不按注册顺序决定。
- 插件缺失后项目重开：记录及字节保留，已有其他资产仍可使用。
- 外线程、回调内关闭、读取中重载、旧 Session/View 代际：拒绝路径不改源与 owner。
- 在任务／窗口／错误载荷仍存活时请求卸载：明确延迟／拒绝，不提前释放代码。
- 只安装 SDK：不能 include ApplicationImpl、pinclude/sinclude 或借旧 build DLL。
- 查看宿主 diff：除通用关系／能力的实现外，不能有 skeleton 类型名、枚举分支或专用 getSkeletonService。

验收示例可以位于独立 consumer 工程，但必须通过真正 DLL／版本协商使用通用宿主。不能直接把测试 TU 链入宿主伪装成插件。

## S7：规范、删除和最终交付

### 删除交叉清单

| 项目 | 必须清除／替代 |
|---|---|
| History | 旧执行成员原体、旧权限、旧消费者；不得有双算法。 |
| 资产路由 | 生产路径中的旧 closed kind 三类表；v1 只读迁移表保留。 |
| 视图绑定 | Application 对每工具 Interaction/Preview 的硬编码；通用关系替代。 |
| PreparedSessionData | 旧名与 API 迁移残留，不留转发别名。 |
| 快照与请求 | 可改 owner-dependent fields、动作与无关 target 配对。 |
| 贡献描述 | 为取得描述而依赖完整具体 View 的公共头关系。 |
| 适用性 | 与统一查询并行的全 schema／全 Feature UI 旁路。 |
| 复杂度 | 被新索引替代的重复线性扫描和死成员；不能只新增缓存不删旧扫描。 |
| 安装与代码生成 | 旧头／包 alias／生成路径／导出版本、消费者全部同步。 |

### 最终资格

- 按 AGENTS 做 tracked snapshot 检查、固定 clean commit、全量 `all -j 4 -- -k 0`；CMake 改动后第二轮无新增工作。
- 原 History、三作者模型、P05/P11 R1、Run、Host、读写／文件冲突、实例代际与代码寿命的受影响行为保持。
- CPU/PLAYER 隔离和实际禁止依赖正反夹具；新公共头按 provider 独立安装／编译。
- 真实骨骼 DLL 的三路线；三种内置编辑器通过同一开放路由的回归。
- 与变更相关的既有双视口／输入／GPU 资源路径，不把 mock 当 GPU；不重启无关长测。
- Linux、系统 IME、已免验旧人工链和旧性能 PARTIAL 原样保留；不擅自变成通过。

### 24 个验收主题（不是测试程序数量）

| ID | 观察 |
|---|---|
| XEC-01 | 新数据／执行边界真实，旧 History 变异算法已迁出。 |
| XEC-02 | Undo/Redo/NO_CHANGE/预算失败保源、保日志、保 redo。 |
| XEC-03 | 不同执行者对同历史重入仍拒绝；外线程错误准确。 |
| XEC-04 | 输入、plan、retired、闭包、错误与代码在正确顺序释放。 |
| XEC-05 | 无 UI 的资产查询与冻结读取 consumer。 |
| XEC-06 | 原 Pane 的独立构造、登记、挂载与关闭 consumer。 |
| XEC-07 | 内置与插件使用相同的能力注入，无私有业务后门。 |
| XEC-08 | 开放类型 Manifest、旧格式读取、未知 provider 保留。 |
| XEC-09 | 路由发现、多个编辑器和歧义处理。 |
| XEC-10 | 通用打开、保存、重载、SaveAll、关闭和恢复。 |
| XEC-11 | 内容发布成功但显示失败的部分完成。 |
| XEC-12 | 一内容多视图、复合视图多内容、独立工具无内容。 |
| XEC-13 | 骨骼实际字段、原 codec、Undo/Redo、重开字节语义。 |
| XEC-14 | 插件缺失／撤销、任务与窗口存活、真实 DLL 寿命。 |
| XEC-15 | 2D/3D/混合/无层级的候选与最终准入。 |
| XEC-16 | 已加载但未声明组件、未知既存数据保留。 |
| XEC-17 | Feature 依赖/冲突/设备需求仍用原规则。 |
| XEC-18 | 能力查询的版本失效与固定目标，不读全局焦点。 |
| XEC-19 | 快照不可破坏 owner 关系，同快照索引正确。 |
| XEC-20 | 无效动作／载荷不可构造，所有原合法动作仍可表示。 |
| XEC-21 | Outliner／模型目录核验的操作计数不再相乘增长。 |
| XEC-22 | 快照／投影成本和检查有效期有准确记录；无虚构加速。 |
| XEC-23 | AGENTS 修改闭包检查、独立头、真实依赖和生成同步。 |
| XEC-24 | 同一最终提交的行为映射、删除结果、支持／免验范围。 |

## 2. 改动候选路径（必须由 S0 展开真实调用）

```text
editor/editing/include/.../EditHistory.hpp
editor/editing/include/.../EditOperation.hpp
editor/editing/src/history/EditHistory.cpp
editor/authoring/{scene,material,flow}/...Session*
editor/authoring/project/{ProjectManifest,ProjectCatalogModel} 对应头与 CPP
editor/activities/project/ 的源读取、项目登记、导入与发布
editor/activities/sessions/ 的工厂、安装与内容操作
editor/activities/persistence/ 的真实保存／发布入口
editor/activities/scene/ModelCreationOperation 与投影活动
editor/workbench/desktop/ 的窗口工厂与现有 Host
editor/workbench/{scene,material,flow,project}/ 的消费者
editor/application/{EditorViews,EditorSaving,EditorRecovery,EditorArtifacts} 等组合
editor/application/extensions/ 的导出、贡献与内置登记
engine/modules 的实际需要补充的纯描述和 Skeleton 命名消费者
cmake 的实际组件、安装、SDK consumer 和架构规则
```

省略号表示从源码展开，不表示可以创造不存在的路径。新类型归现有主题；除必要独立 consumer 外不创建新顶层。

## 3. 阶段交付格式

每批交接说明：实现 SHA、实际工作区、用户补丁状态、已迁移责任、已删除旧项、尚未迁移的真实消费者、测试命令和结果、未测／免验范围。

最终实现与验收分别提交。报告结论按以下区分：源码已实现、静态风险已修、真实 SDK 已验证、继承证据、未运行、用户免验。没有运行的外部骨骼完整路径不能写成“插件扩展完成”。

不要再生成多套重复证明脚本。现有 stage/STRICT 检查器扩展 EC1 授权和依赖规则，保证旧阶段仍按旧 SHA 核验；不能把 `P13` 的标签继续用作 EC1 新代码的通过凭据。

## 4. 结束而不继续膨胀

EC1 完成后，架构应当更少认识内置具体类型，用户得到更多可单独使用的正式能力，而不是更多抽象层。

没有明确用户功能需求时，不继续追加：任意 Graph 插件序列化、全部投影增量、任意热卸载、完整动画工作台、无限可配置 capability DSL 或全平台 CI。发现真实正确性问题必须说明；纯粹“还能更抽象”不构成新增阶段。
