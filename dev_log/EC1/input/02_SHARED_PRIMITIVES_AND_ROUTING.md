# 02　同一套细粒度能力：内置与外部共同使用

## 1. 不是发布一个 IEditor，而是发布可组合能力

本轮不以 `drawEditor(context)`、`IAssetEditor` 或一个巨大的 `EditorServices` 作为全部 SDK。

开发者应能只使用需要的子集：无 UI 的资产检查工具；有 Pane 但不打开作者内容的资源浏览器；一份内容多个视图；一个视图同时比较多份内容；有独立历史并参与保存的完整骨骼编辑器。

这些用法不应要求插件自行管理第二 SessionStore、直接修改项目 TOML，或通过 Application 私有成员取得对象。

## 2. 能力目录与实际 provider

| 能力 | 现有基础／归属 | 公共契约要保证什么 | 不应暴露什么 |
|---|---|---|---|
| 资产身份与种类 | modules 的 AssetId、AssetTypeId；authoring/project 描述 | 稳定种类不依赖闭合 Editor enum；来源与派生产物种类区分。 | 运行时 Entity、函数地址、C++ type hash 作为磁盘 ID。 |
| 查询项目资产 | ProjectCatalogModel / ProjectCatalogSnapshot | 同版本不可变数组和索引、准确项目实例、按 ID 查询、变化通知。 | 可被调用者改写的 span/version、目录私有容器。 |
| 读取作者源 | ProjectStorage::captureSource、AssetVfsView | 固定目标、版本／摘要、预算；读取失败不是不存在。 | UI 回调中的同步整文件读取、未受控的 ProjectStorage 内部表。 |
| 读取 cooked 资产 | captureAssetReads、AssetReadPort、Process loadAsset | 使用固定读取端点、真实 codec、取消与拥有型完成。 | 与作者源码混用的无类型字节入口。 |
| 创建／访问会话 | SessionStore reserve/prepare/publish、TSessionAccess | 唯一 owner、完整代际、读写准入、不可见准备、真实角色安装。 | 任意 Registry 可写指针、第二 current/dirty。 |
| 编辑与历史 | 领域 Session / EditExecutor / EditHistory | 领域准备、一次提交、Undo/Redo、冻结输出；也支持无窗口使用。 | UI 直接改变 cursor、执行任意旧 memento。 |
| 创建 Pane 与 Element | modules/function/ui，LuxObject | 真实离树构造、原控件布局与信号；不要求继承额外插件 UI 基类。 | 第二 Root、插件私自驱动消息泵。 |
| 注册窗口种类 | 正式 ViewFactory、ContributionDraft | 类型／版本／绑定、代码寿命、一次构造结果；支持独立工具窗口。 | 固定骨骼/Scene 分支或与主菜单字符串耦合。 |
| 挂载／请求窗口操作 | ViewHost、ViewRequests、IViewHost | 创建、采用、描述、show/focus/close；owner 安全点；结果与请求分开。 | 任意回调立即删除自身、返回无限期裸 Pane 借用。 |
| 命令 | CommandRegistry 与不可变目录 | 目标固定、query/execute 分开、旧版本 pin、后批更新。 | 为获得一个方法而暴露整 Application。 |
| 后台执行 | engine/process | 原 TaskScope、scheduler、取消、进度和完成吸收。 | std::async、自有线程池、新任务状态数据库。 |
| 保存与文件发布 | SaveService、SaveExecution、WriteCoordinator | 源保存、产物发布、项目目录采用各有结果；同物理目标排序。 | 直接 ofstream/remove 绕过在途写入。 |
| 投影／预览 | 原 Runtime、RenderResources、viewport 与领域投影活动 | 使用既有资源与退休；固定来源、视口本地状态。 | 第二 RenderRuntime 或 Window 退出即删共享运行实例。 |
| 布局／内容恢复 | 纯 LayoutPlan、ViewHost、WorkspaceStore、内容打开活动 | 布局只组织视图；内容从独立记录恢复。 | opaque 布局载荷默认授权打开任意资产。 |

以上必须有独立的安装头和最小 consumer，不意味着每行必须新建一个类。现有 API 已够用时直接复用。

## 3. 读取、任务与 owner 的边界示例

目标使用方式如下。它展示职责，不是逐字可编译的当前 SDK 示例：

```cpp
auto catalog = asset_catalog.snapshot();
auto source = source_reader.capture(asset_id, expected_digest, limits);
auto work = task_scope.submit(read_and_decode(std::move(source)));
// worker 只产出拥有型解码结果。
// owner 收到后通过原 SessionStore / 安装协议发布。
```

不能为了少一层，把 `source_reader` 变成存放全项目状态的万能对象。若它只是 ProjectStorage 四个方法的同义转发，应直接使用已有提供者或在提供者内提取真实的读取职责。

返回借用值的有效期必须写在接口上：同步 callback-only、到下一次 mutation、或由 owning snapshot 延长。`function_ref` 不可跨帧保存。返回 span 的快照必须使所有观察始终指向同一个私有 owner。

## 4. 服务如何交给插件：显式注入，不是查全局 Context

### 4.1 两个时点

**登记时**描述能力、格式、工厂、命令和需求，不创建活动 Pane，也不从当前焦点读取内容。

**激活／构造时**由 application 把所需正式服务引用交给插件的激活对象或工厂。激活对象负责自己的 Connections 和 TaskScope 请求，但服务事实仍由原 owner 管理。

当前 V7 出口没有这一显式 host-services 入口。[S10] 必须在现有扩展契约内演进，而不是通过静态全局变量、反射找私有地址或回调先保存 `EditorApplication*`。

### 4.2 有界依赖集合

可以采用几组有明确归属的组合值，例如：

- 活动层组合：资产查询／读取、SessionStore、保存与原 Process。
- 工作台组合：dispatcher、ViewRequests／必要的 host 准备入口、正式窗口注册与命令接线。
- 编译／预览扩展另外明确请求相应现有 provider，不要求所有插件依赖 GPU、LLVM 或完整 application。

这些是明确字段的构造参数组，不是 `get<T>()`。每个模块只接收自己需要的一组；原有具体 provider 无需再包装成接口同名副本。

可以用编译期 `requires` 校验内置工厂需要的字段；运行时插件通过声明版本和实际 ABI 的窄绑定。不得构造一个无界能力服务定位器，再用“可扩展”合理化它。

### 4.3 生命周期

application 拥有实际 provider，随后构造 extension activation；插件产物的 code lease 必须覆盖对象、错误值、闭包、deleter 与弱引用控制块尾部。

停止／卸载顺序：拒绝新业务 → 保留并结清已接受工作 → 卸下相关视图与连接 → 回收 payload → 释放 activation 和代码。无法安全热卸载时明确拒绝／延迟，不要求本轮实现任意热卸载。

进程内能力注入是接口和生命周期约束，不是对恶意原生插件的安全沙箱。不得宣称给予一个窄引用即可阻止插件调用 OS。

## 5. 普通窗口不必成为“作者编辑器”

插件可以直接用原 Pane／Element 组合自己的窗口。只有希望参与内容关联、SaveAll、关闭内容和恢复时，才登记内容关系。

关系至少支持：

| 场景 | 所需关系 |
|---|---|
| 任务窗口、统计窗口 | 无 Session 关联 |
| 同骨骼两视图 | 一个 Session、多 ViewId，各自视图状态 |
| 对比两个骨骼 | 一个 ViewId、多个只读／可编辑内容关联，明确主操作目标 |
| 工厂构造成功但挂载失败 | DetachedView 仍由完整 owner 清理，已发布内容不被错误回滚 |
| 关闭一个视图 | 只结束该视图的交互和关联，不自动关闭 Session 或取消未拥有的任务 |

宿主保存通用关联身份与关闭策略；具体骨骼交互、相机、选择和预览绑定由该实现管理。必要的辅助 owner 可放进已有 DetachedView 的完整拥有单元或工厂结果，避免在 Application 再加一个 Skeleton 指针。

活动层不得为此 include ViewHost。跨内容和 UI 的编排仍在 application/workbench 的既定方向。

## 6. 开放路由：补关系，不再造万能文档模型

目标关系为：

```text
稳定来源资产类型 + 格式版本
    → 作者会话工厂
    → 默认保存命名／项目登记政策

会话种类 + 上下文需求
    → 可用内容视图工厂（可多个）
    → 绑定准备器 + 视图状态 codec
```

目标名可为 `AuthoringTypeRegistration` 和 `ContentViewRegistration`。实现前检查已有条目能否直接补充这些关系；不能同时保留一张新表和旧 `assetKind()` 人工表。

内置 Scene、Material、Flow 也登记同样关系。Application 只负责查询、歧义处理、固定版本、调用工厂和协调结果，不再识别各个 C++ Session 类型。

### 6.1 多重候选不能“第一个获胜”

同一来源有多个编辑器时：默认选择必须在描述或用户偏好中明确；没有唯一默认时返回候选，用户显式选择。不能依靠注册顺序、文件名排序或随意最高 priority 决定打开结果。

文件后缀是发现线索，不是唯一类型依据。持久 type、格式版本、codec／header 验证保持。TypeToken 仅用于同 ABI 内绑定检查，不写入文件。

### 6.2 来源格式与 runtime 资产类型

复用已有 `AssetTypeId`，不要新增 `EditorAssetTypeId` 表达同一件事。[S34]

但作者材质图与编译后 MaterialAsset 不是同一个格式；它们可以使用不同的规范类型名，描述关系明确连接。`SessionKindId` 则表达会话实现种类，也不与 AssetTypeId 混用。

### 6.3 Manifest 演进

当前五值 kind 不能容纳未提前列举的作者资产。[S04,S05]

本轮允许为开放类型增加一个明确的新 Manifest 版本：持久记录使用规范类型名、来源格式版本与必要的已有字段。旧 v1 只读解析为等价新内存表示；**仅在显式保存／发布时写新格式**，不得打开项目就覆盖旧文件。

稳定 hash 必须与规范名核对，不能默认不存在 hash 碰撞。未知但语法合法的类型记录保留并显示“缺提供者”；不把整个项目判成无效，不丢用户数据。无法理解的旧版本／非法字段按原严格策略拒绝，保留字节，不猜测。

旧布局、资产实际 codec 不因 C++ 类型搬家随意升级。没有实际格式必要的地方保持原版本。

### 6.4 派生产物

具体 MaterialCompiler、FlowCompiler、Skeleton 编码器保留各自领域输出。到共享文件发布边界时交付：拥有型字节／编码工作、AssetTypeId、来源戳、目标与编码版本。

不要在 Application 中增加 `SkeletonCompiled` 到 `VCompiledSource`。也不要抹掉所有编译类型变成裸 `any`。擦除只发生在真实异构保存／发布边界，持有 code 与完整 provenance。

## 7. 原有基础接口不足时允许真正重构

需要检查的不足包括：

- IViewHost 目前只有 describe 加 ViewRequests，完整 adopt／批量准备在具体 Host。选择继续暴露真实公开 ViewHost，或给原接口补所需最小操作；不能新建一个逐项转发 HostAdapter。
- 读取必须能够捕获 owning 端点，而不是要求插件持有整 ProjectStorage。若现有方法足够，则仅传相应端点。
- content-view 工厂须取得构造需要的公共会话能力，不能要求 Application 先构造每一种具体 Interaction。
- 关闭／重绑定涉及 payload 析构时，公开协议必须表达可重试与永久失败，而不是返回 bool 丢失事实。

细粒度不是无限分散；最小使用单元由真实职责决定。同一 provider 的紧密相关方法放在一起，普通算法不用虚接口。

## 8. ABI 与安装

这是公共插件契约变化，不允许在保持旧 V7 签名／结构大小的同时偷偷改变含义。根据最终变更更新 Editor 导出版本／ABI 指纹（预期需要下一版本，具体以已有机制确定），在调用前拒绝不匹配模块。runtime 插件 ABI 不因纯 Editor 改动无故变化。

公共头通过实际 provider 安装；不得对外发布 pinclude/sinclude 或整份 ApplicationImpl。内置与插件统一使用这些公开入口。旧 API 删除时同步所有源码、生成器、安装消费者，不保留 namespace alias、转发头和 Old/New 回退。
