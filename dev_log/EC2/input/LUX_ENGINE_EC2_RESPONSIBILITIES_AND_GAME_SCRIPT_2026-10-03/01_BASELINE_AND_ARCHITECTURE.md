# 01　新基线、问题差异与架构裁定

## 1. EC1 结果继承：不能再按旧报告施工

| 上一次调查中的事项 | 新基线中的状态 | EC2 行动 |
|---|---|---|
| History 承担执行 | EC1 报告已变为 EditHistoryData + EditExecutor | 核验原回归与调用，保持，不再改回另一名字。 |
| PreparedSessionData | 报告已由 SessionPreparation 替代 | 保持一次消费与代码寿命；不新增转发执行者。 |
| EProjectAssetKind / 三类型路由 | 报告已引入 SourceAuthoring、开放工厂与 manifest v3 | 核验并继承。运行期脚本不得反过来依赖作者路由。 |
| ProjectCatalogSnapshot public 借用字段 | 报告已私有化并加同快照索引 | 使用现有快照，不再新增 CatalogReader 或第二索引。 |
| VCompiledSource | 已由 DerivedArtifact / IArtifactSource 取代，当前 EditorArtifacts 已使用新输入 | 开放输入已完成；**执行流程仍在 Application**，只迁剩余责任。[C04] |
| ResultIntent action+target | 当前 ResultsPane 已用 VResultIntent，动作绑定载荷 | 保留；面板仍持 Impl&，迁移此依赖。[C09] |
| 组件/Feature 适用性 | EC1 已有 AuthoringFacts/queryApplicability，当前配置 UI 已调用 | 复用，不再造第二要求解释器；从 UI 提取纯准备时继续使用。[C08] |
| Outliner/目录乘法扫描 | EC1 报告已整改 | 不为本轮再实现一套集合；只在相关变更时测回归。 |
| Scene 全快照重建 | EC1 明确保留 | 不把增量投影追加成本轮必选。 |

以上分别为当前读取源码事实或 EC1 提交报告事实，详见 [C03–C09]。报告不等于本次独立运行结果。

## 2. 新基线中仍应整改的责任

| ID | 当前事实/证据 | 真正需要改的东西 | 不应误改 |
|---|---|---|---|
| RD01 | ProjectPublication public 可变计划 + private owner 指针，头与旧版本相同。[C05] | 固定计划与 owner-thread 占用语义；计划不能准备后随意改写。 | ProjectWriteLease 的 RAII、原发布序列和已发布事实。 |
| RD02 | SceneConfigurationElement 公开 build/buildEdit，持 UI+builders+注册输入；CPP 仍含 presetFeatures、systemOptions。[C07,C08] | 草稿与纯准备，UI 不再成为唯一配置编译入口。 | 原 Builder 校验、未知配置、当前 AuthoringFacts。 |
| RD03 | DerivedArtifact 已开放，但 settleArtifacts 仍在 Application 排 Task、encodePak、发布与目录采用。[C04] | 发布业务和进度归原活动；Application 只接纳产品意图/观察。 | 已完成 IArtifactSource，不能重建第三套产物接口。 |
| RD04 | ResultsPane 已使用类型化意图，但依然直接读 writes/sessions/runs/Impl。[C09] | 正式数据观察及请求能力，面板独立构造。 | 合法 UI 回调延迟执行业务的原安全点。 |
| RD05 | MaterialPreviewStore 仍接收 MaterialCompileOperation；key 仍带 target；preview 返回 CompileResult。[C06,C10] | 配方、编译输入、目标采用、错误域分开，保留完整 live owner。 | GPU/Runtime retirement，不把预览拆成散落裸句柄。 |
| RD06 | WorkspaceStore 仍包含迁移、回退、layoutResult+catalog。[C11] | 纯转换/选择与真实读写分离，查询不能伪装成提交回执。 | 真实 IO 版本复查与 marker 幂等。 |
| RD07 | 旧调查指出模型 recipe codec 埋于 AssetImporter。[U02] | R0 复核当前完整实现后提取，明确 ModelImport 领域。 | 已有 ModelCooker 和 Process，不造万能导入器。 |
| RD08 | ProjectOpenData 带 write lease（旧调查）。[U02] | R0 复核，必要时改为拥有型准备名称并封装。 | 不要求可复制，更不能将活 Project 交 worker。 |

RD07/RD08 本次未重读完整实现，不能写成已取得新运行负例。实施者先核对它们的实际剩余工作。

## 3. 架构现在是否清楚

目录方向已清楚；职责仍有几处横跨多个实际变化理由。因此不重新设计五层，而要完成三种边界：

- **表示与处理**：draft/plan/immutable result 与 Builder/codec/operation 分开。
- **准备权限与业务数据**：permit、reservation、code lease 不是普通 DTO，不和可改字段混装。
- **宿主装配与可复用业务**：Application 可以组合，但不要自己实现另一个发布器或让通用面板读取全部私有状态。

新加入的游戏脚本让第四种边界必须明确：**运行能力与 Editor 产品能力不同**。

## 4. 三条消费链，不是同一万能 Context

```text
Editor 内置工具 / Editor 扩展
    -> EC1 的 SessionActivities / ProjectActivities / WorkbenchAccess
    -> Editor 作者与工作台提供者
    -> engine / modules 的原能力

C++ 游戏代码
    -> 运行期资产读取、组件、时间等公开能力
    -> Process / VFS / codec / Simulation

Lua / 已有原生脚本后端
    -> ScriptAbility 声明与语言投影
    -> 同一运行期能力 + 脚本调用的寿命/额度
    -> 原 Process / VFS / codec / Simulation
```

第二、三条路径都不得绕入 Editor。把 Lua 包一层 `ProjectActivities` 不叫复用基础设施，而是把游戏运行期绑到编辑器。

“同一接口”要求业务契约、结果含义和真正执行算法一致；不要求 C++ 的 `shared_ptr<const Asset>` 和 Lua 的稳定句柄具有同样的内存布局。语言投影不是应删除的兼容补丁，它有真实表示与寿命责任。

## 5. 层级与模块归属

| 责任 | 正式归属 | 依赖限制 |
|---|---|---|
| AssetId/AssetTypeId、资产布局、codec | modules/resource 原提供者 | 不含 Editor/ScriptRuntime/VM。 |
| AssetVfsView/AssetReadPort/loadAsset | modules/resource 与 engine/process 原提供者 | 不知道 Pane、History、Lua。 |
| ScriptAbility 契约/语义/生成，通用 Lua 值运输 | modules/function/script 原 core/lua | 不识别 Skeleton/Material/Editor 项目。 |
| ScriptSystem、awaitable、组件命令屏障 | engine/domain/simulation 原 scripting/builtin script | 不引入 Process 文件后端或 SceneRenderer 具体实现。 |
| 资产能力的运行期实例作用域与组合 | **目标：engine/scene/scripting/assets/** 的小型叶子接线模块 | 组合 process + script 协议；不反向让二者依赖该叶子。不得依赖渲染/GUI 才能读取。 |
| Lua 资产投影 | 同主题独立可选 target，或已有合法的相邻语言投影 target | native 合约不因 Lua 引入 lua.h；不是整个新脚本引擎。 |
| Editor 发布/草稿/预览/工作区 | 原 authoring/activities/workbench | 不成为游戏脚本 ABI。 |
| EditorApplication / PLAYER / 测试宿主 | 各自装配根 | 安装同一 runtime provider；不各写业务回调算法。 |

`engine/scene/scripting/assets` 是**拟议新叶子位置**，当前树中未取得这个目录已有实现的证据。R0 若发现已有同职责正式模块，应并入它并删除该新增计划；不得两处实现。放 scene 组合层是为了避免 domain/core 为访问文件反向依赖 process；不要求对象必须成为一个新的 SceneSystem，也不要求依赖 SceneRuntime 重资源。

源目录不是动态库数量。默认 native 能力接线一个 STATIC target、Lua 投影一个可选 target；若真实公开协议需独立 CPU 头 target，必须由实际无 Lua/无 Process 消费者证明。不得每种 handle 建一个库。

## 6. 游戏能力清单的分类方法

R0 从公开 provider、ScriptAbility 声明、生成列表、实际调用和注册位置建立清单，至少包括以下类别：

| 能力族 | 本轮定位 |
|---|---|
| 资产读取/解码/结果查看/释放 | 完整新实现与接通的主切片。 |
| 运行对象/组件读取与写命令 | 复用 DeferredScriptHost/原 typed component 绑定，至少一条真实组件用例。 |
| 时间/延时 | 复用 DelayAbility，作为另一能力独立组合的对照。 |
| 空间查询 | 盘点真实同步/异步、可用性和结果合同；本轮不强制新增完整 raycast 绑定。 |
| 音频、物理、输入、动画等 | 逐项标记已有绑定/缺绑定/不在范围；不由“可反射”推导“可安全脚本化”。 |
| 作者导入/编译/保存/布局/Pane | Editor-only，排除游戏脚本导出。 |

能力的可用性来自宿主明确提供和脚本声明匹配，不来自遍历全局 Application、DLL 符号或字符串查找服务。EC1 AuthoringFacts 留在作者编辑语义；ScriptApiCapability 继续表达运行期协议，两者不能混为一个 Is3DManager。
