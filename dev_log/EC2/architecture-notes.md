# EC2：责任、寿命与删除对照

这份记录属于原 `.internal/editor-redesign/EC2` 施工节点。最终运行结论以 receipt、coverage、commands 及其日志为准；本文不把源码审查当作运行证据。

## EC1 补充项与本轮落点

| 审计项 | 原责任混合 | 本轮唯一位置与消费者 |
|---|---|---|
| RA01 | publication 值同时携带可改计划和 Storage 占用权 | `ProjectPublicationPlan` 共享不可变数据；`PreparedProjectPublication` 独占原 Storage reservation。`ProjectPublicationOperation` 消费准备结果；worker 只读计划。 |
| RA02 | SceneConfigurationElement 内含配置构造算法 | authoring/scene 的 `SceneConfigurationDraft` 保存值；activities/scene 的 `SceneConfigurationPreparation.cpp` 执行唯一准备算法。UI、无窗口 SDK、注册贡献走同一入口。 |
| RA03 | Application 执行 cooked 资产打包与发布状态机 | activities/project 的 `ArtifactPublicationOperation` 组合原编码任务、`ProjectPublicationOperation` 和同一个 WriteCoordinator。Application 只接纳意图和观察结果。 |
| RA04 | ResultsPane／WorkspacePane 借用整个 ApplicationImpl | workbench/project/tools 的 `ResultsView`／`WorkspaceView` 借用明确的只读观察与类型化请求。原内部 Pane 定义删除。 |
| RA05 | 编译 key 夹入单一预览目标 | `CompileInputKey` 描述固定输入；`PreviewAdoptionKey` 包含目标代次、输入、配方和环境。`MaterialPreview` 是活资源 owner，`MaterialPreviewRecipe` 是共享输入。 |
| RA06 | Workspace 查询隐含刷新 IO，迁移读取和转换混在一起 | `LegacyWorkspaceInput` 固定读取内容；纯转换保留 selected-only／marker 规则；`WorkspaceMigration` 捕获、复查并使用原 Store/Coordinator。状态查询不刷新目录。 |
| RA07 | AssetImporter 名义通用，实际只做模型 | 改为 `ModelImporter`；`ModelImportRecipe` 编解码只处理字节。原 ModelCooker、IO、重导入及取消协议继续共用。 |
| RA08 | ProjectOpenData 名称掩盖拥有的准备与 lease | `PreparedProjectOpen` 明确拥有固定打开输入；旧头、定义及全部调用替换。 |
| RA09 | CompiledMaterial／CompiledFlow 可拆散来源与编码 | 私有固定数据和公开观察保持来源、产物、字节的关联；可复制结果，不可复制 operation 控制权。 |
| RA10 | 已完成 History／Snapshot／SessionPreparation | 不重建。保留 EC1 EditExecutor、唯一 History、开放内容路由、V8 和外部 Skeleton。 |

## 五条实际执行链

### 固定项目更新与发布

`ProjectUpdate → ProjectStorage::preparePublication → PreparedProjectPublication → ProjectPublicationOperation → WriteCoordinator → 文件与 manifest → Storage 采用`。

计划可以共享读；唯一准备 owner 管理原 reservation。worker 无 Storage 指针和解除占用权限。原发布前版本、receipt 身份、Unknown lane、部分磁盘成功和采用校验继续存在。取消不抹去已发布事实；重试不会重复写已成功产物。存储格式未因职责移动变化。

### 配置准备

`现有描述／控件值 → SceneConfigurationDraft → prepareSceneConfiguration[Edit] → 具体 Scene 领域提交`。

纯 draft 不持 Root、控件或 Registry。已有 opaque 配置依原来源保持；缺插件不能伪造新配置。provider、依赖、版本和不支持的 partition 仍在真实准备边界拒绝。结构修改能力没有被扩张。

### 编译、预览与退休

`固定作者捕获 → 原编译 operation → 不可变 CompiledMaterial／CompiledFlow → 指定目标的采用 key → MaterialPreview／原 RenderResources → 原 retirement`。

编译产物不拥有窗口或 target；两个目标独立验证自己的期望输入、代次和配方。编译失败和预览失败分别报告。关闭视图不代替异步或 GPU owner 确认退休；最后成功呈现按原规则保留。没有第二 Renderer、发布器或资产缓存。

### 运行期脚本资产

`ScriptRuntimeHost 明确注入固定 AssetReadPort → 原 ScriptSystem 准备实际 ScriptInstanceId → ScriptApiInstanceBinding → ScriptAssetScope → 原 portSender／loadAsset<T>／TaskScope → 固定 native outcome → 原 completion ingress → 原 ScriptAbility/LuaBoundary 恢复 → 显式 release 或 scope revoke`。

ScriptAssetAccess 只拥有原 TaskScope 和有界实例记录；每个 scope 保管此实例的有界请求及结果。worker 拿固定输入、codec 和任务结果，不借 Registry、VM 或作者 Session。维护可以接收暂停期间的结果；规则仍由 Simulation 图执行。BACKPRESSURE 时原有界记录保留完成事实，不重发 IO，不绕过 VM 准入。

句柄是 scope＋slot generation 的别名；复制句柄不会复制资源 owner。释放使所有旧别名失效。卸载顺序为撤销 scope、销毁 backend、销毁 binding、释放脚本 code；scope 回收不依靠 Lua GC。typed 结果先析构资产再释放插件 code，控制块由 native DLL 分配，避免在插件控制块返回前卸载代码。

`ScriptRuntimeHost` 仍借用 backend、描述 span 和 resolver context，组合方必须使它们活过实例。RunEnvironment 的 shared host 只延长 host 本身，不暗中拥有这些借用。没有自动 Editor 服务查找。

### 活动与显示

`原 owner 的只读结果 → ResultsSnapshot／WorkspaceSnapshot → 独立 View → owning typed intent → 原安全点`。

BUSY／IO 保留上次有效显示并呈现原因，不能当作空结果。View 销毁不确认它未拥有的 operation。现有 TaskMonitor、Host、Root 继续原职责。

## 原生能力与语言表示

- 原生 `scene_script_assets` 不依赖 Editor、Lua、GPU、LLVM。SHARED 边界仅保证跨 DSO 共用一个 ScopeIdSource。
- `AssetAbility` 提供原始资产读取、固定长度字节查询与显式释放；`SkeletonAbility` 是独立 typed 能力，未修改通用 VM 的资产分支。
- 可选 `scene_script_assets_lua` 使用正式生成器和原 LuaValue／ScriptAbility 投影。完整 ID 使用有界 opaque userdata，不经浮点数、不含资产 owner 或指针。
- 外部 async resume 只允许满足原语义／布局且有界的 trivial 值；非平凡 C++ 资产留在 native scope。原 Delay、LuaBoundary 和 deferred component 命令屏障不另建。
- 不自动开放任意资产类型、渲染资源、物理、音频或全部 ECS。只声明本轮实际绑定能力。

## 检查收敛与必须保留的边界

| 已建立事实 | 有效范围 | 仍需重新检查 |
|---|---|---|
| 不可变 publication plan 的关联 | 同一计划的同步读取及拥有型 worker 输入 | 发布前文件版本、回执、原 Storage 采用身份 |
| compiled result 的来源与编码 | 同一个不可变结果的多个读者 | 每个 preview 的目标、代次、输入、配方、环境 |
| withAsset 已取得 typed owning read | 本次同步 noexcept 回调 | 另一次查询的 scope/slot/type；回调后访问重新查槽 |
| 脚本冷准备已解析方法／值 codec | 此 binding 和 script instance | 外部 Lua 参数、schema、值大小、实例及资源代次 |
| 原生工作已接受 | 直到完成可靠交付或原协议结清 | scope 是否已撤销、ingress 是否背压；不得用新业务 BUSY 丢完成 |

没有删除文件冲突、来源戳、Session gate、跨 await 代次和 callback 后查槽。没有新增 OOM 恢复、dispatch 热路径 catch 或观察者内直接改世界。

## 保护与范围

用户应查看 `E:/SyncForder/CodeRepos/lux-engine-ec2`，干净资格检出为 `lux-engine-ec2-qualified`。原工作区及 main 不动。ProjectBuilder 用户修改未应用；对应新路径为 `editor/authoring/project/src/ProjectBuilder.cpp`，原归档继续保留。

Linux、系统 IME、Android 构建没有启动；P12 `PARTIAL_USER_WAIVER`、旧 50k 慢样本 PARTIAL 和所有历史快照不改判。EC2 新 Windows／Lua／PLAYER／SDK 的结果单独记录，不能借历史免验替代本轮必测。
