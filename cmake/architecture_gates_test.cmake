# =============================================================================
#  architecture_gates_test.cmake — 把 ECS/渲染/执行/扩展/生命周期已经退场的
#  机制钉成零容忍防回归门禁。
#
#  活跃 SSOT：
#    .internal/runtime-extension-system-design.md
#    .internal/event-system-design.md
#    .internal/render-system-upload-pipeline-design.md
#    .internal/render-resource-system-design.md
#
#  ── 为什么仍保留“计数”实现 ───────────────────────────────────────────────
#
#  表结构允许个别尚在迁移的门禁临时使用非零上限；当前列出的退场机制目标和
#  上限都必须是 0。任何命中都表示旧机制、兼容 shim 或第二条公共路径重新出现。
#
#  ── 上限怎么维护 ──────────────────────────────────────────────────────
#
#  · 计数 > 上限  → **失败**。要么是新写了旧机制,要么是真需要提高上限
#                   (那需要在这里写明理由,不能默默改数字)。
#  · 计数 < 上限  → 通过,但打 WARNING 提醒立即收紧；活跃退场门禁通常不应发生。
#
#  不做成「必须精确相等」是因为一批施工中途计数会反复波动,那样 ctest 在
#  整个开发过程里都是红的,红久了就没人看了。
#
#  ── 调用 ──────────────────────────────────────────────────────────────
#      cmake -DREPO_ROOT=<仓根> -P cmake/architecture_gates_test.cmake
#  由根 CMakeLists 无条件 add_test(不挂在任何 ENABLE_*_TEST 下 —— 门禁的
#  意义就在于没人能把它关掉)。
# =============================================================================

if(NOT REPO_ROOT)
    message(FATAL_ERROR "architecture_gates: 必须传 -DREPO_ROOT=<仓根>")
endif()

# 扫描排除:测试(示例代码,允许用旧机制做对照)、第三方、构建产物。
set(_exclude_regex "/test/|thirdparty|/build/|engine/editor/framework/native_dialog/src/")

# ── 门禁表 ────────────────────────────────────────────────────────────────
#
# 每条:id / 正则 / 扫描根(相对仓根)/ 当前上限 / 终态目标 / 归属批次 / 处方
#
# 正则用 CMake 正则语法(ERE 近亲):无 \b \s \d,括号与点要写成 \\( \\. 。

set(_gates
    deferred_commands
    legacy_render_runtime
    legacy_upload_pool
    legacy_async_runtime
    legacy_execution_mechanisms
    legacy_event_workflow
    legacy_extension_bundle
    legacy_component_registry
    registry_ctx_boundary
    legacy_world_streaming_scan
    legacy_world_runtime_bus
    legacy_world_asset_types
    legacy_world_wire
    legacy_world_resource_model
    legacy_world_cooker
    legacy_scene_plan
    legacy_scene2d_pack
    legacy_scene3d_pack
    legacy_main_thread_names
    modules_upper_layer_include
    ecs_upper_layer_include
    runtime_upper_layer_include
    authoring_upper_layer_include
    entity_scene_domain_leak
    ecs_render_world_leak
    physics3d_world_identity
    legacy_lifecycle_wait
    shutdown_spin_wait
    raw_texture_ownership
    generic_upload_reply_clone
    upload_raw_payload
    business_then
    frame_protocol
    rtti
    exceptions
)

# ── G1 ── registry 持有的任意闭包命令队列。
# 已由 Schedule 的 typed command shard 取代(批 A1,设计稿 §5.1)。**已达标**,
# 这一条从此是防回归:再出现就是有人又往 registry ctx 里塞了一个闭包队列。
set(_gate_deferred_commands_regex  "DeferredCommands")
set(_gate_deferred_commands_roots  "ecs;engine")
set(_gate_deferred_commands_max    0)
set(_gate_deferred_commands_target 0)
set(_gate_deferred_commands_batch  "已达标(防回归)")
set(_gate_deferred_commands_fix
    "改用 Schedule 拥有的 typed EcsCommandBuffer:观察者只写自己的 shard,\n    barrier 在 Schedule::tick 末尾唯一一处 apply。不要新增 registry ctx 队列。")

# ── G2 ── 退役的帧外伪帧运行时不得回来。
set(_gate_legacy_render_runtime_regex  "RenderRuntime")
set(_gate_legacy_render_runtime_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_render_runtime_max    0)
set(_gate_legacy_render_runtime_target 0)
set(_gate_legacy_render_runtime_batch  "已达标(防回归)")
set(_gate_legacy_render_runtime_fix
    "正常帧用 FrameCoordinator；帧外 RPC 用 RenderControlSession，持久资源\n    上传用 RenderUploadSession。不得重建 control frame/ensureFrameOpen 状态机。")

# ── G2b ── 多 worker 上传池及其锁化 command-pool 租还已经退役。
set(_gate_legacy_upload_pool_regex  "UploadWorkerPool|BlockingRingQueue")
set(_gate_legacy_upload_pool_roots  "modules/function/render;ecs;engine;platforms")
set(_gate_legacy_upload_pool_max    0)
set(_gate_legacy_upload_pool_target 0)
set(_gate_legacy_upload_pool_batch  "已达标(防回归)")
set(_gate_legacy_upload_pool_fix
    "上传后端固定为单 GpuTransferPipeline：render→transfer job SPSC 与\n    transfer→render result SPSC；共享 VkQueue 走 RECORD_ONLY，不加提交锁。")

# ── G2c ── 退役的执行器/事件 RPC 名字不得回来。DomainEvents 只承载
# 已提交事实；有 terminal result 的工作必须是 AsyncRuntime typed operation。
set(_gate_legacy_async_runtime_regex
    "(^|[^A-Za-z0-9_])(EngineExecutor|TaskScope|SubscribeAsync|EventBus)([^A-Za-z0-9_]|$)")
set(_gate_legacy_async_runtime_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_async_runtime_max    0)
set(_gate_legacy_async_runtime_target 0)
set(_gate_legacy_async_runtime_batch  "已达标(防回归)")
set(_gate_legacy_async_runtime_fix
    "有结果的后台工作用 AsyncRuntime typed operation + AsyncScope；主线程\n    状态提交后才发布 DomainEvents fact。不要添加旧名 shim 或第二套运行时。")

# ── G2d ── Asio/TBB 收口后，旧中央调度、手写 timer 和通用 detached
# execute 入口不得以新名字重新生长。无返回值意图只能使用受 Value=void
# 约束的 tryNotify；有返回值必须由 execute sender 观察 terminal。
set(_gate_legacy_execution_mechanisms_regex
    "StepPool|setWorkerThreads|RealtimeParallelExecutor|wait_dequeue_timed|dispatchDueTimers|resumeAfter|tryExecuteDetached|EAsyncPriority|CoordinatorWake|bindCoordinatorWakeHandler")
set(_gate_legacy_execution_mechanisms_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_execution_mechanisms_max    0)
set(_gate_legacy_execution_mechanisms_target 0)
set(_gate_legacy_execution_mechanisms_batch  "Execution+Asio+TBB")
set(_gate_legacy_execution_mechanisms_fix
    "异步等待使用 Asio sender，后台 CPU 使用 backgroundCpuScheduler，帧内\n    必须立即得到结果的算法在业务位置原地 oneTBB + join。Value=void 的\n    typed notification 可用 tryNotify，其余 operation 必须观察 execute() 结果。")

# 精确禁用曾经充当工作流/RPC 的领域事件名。不能笼统禁 `Requested`：
# EcsCommandBuffer 内部命令用过去/请求式名字是局部 typed command，不是事件。
set(_gate_legacy_event_workflow_regex
    "(ImportRequested|ImportFinished|CookRequested|CookFinished|MaterialCompileRequested|MaterialCompiled|AssetReloadRequested|AssetReloadFinished|CreateAssetRequested|DeleteAssetRequested|CreateInstanceRequested)")
set(_gate_legacy_event_workflow_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_event_workflow_max    0)
set(_gate_legacy_event_workflow_target 0)
set(_gate_legacy_event_workflow_batch  "已达标(防回归)")
set(_gate_legacy_event_workflow_fix
    "把单消费者命令改成 controller/client typed method；把后台请求改成\n    typed operation。只有权威状态提交后的可选多播事实才进 DomainEvents。")

# ── G2e ── 动态扩展已拆为 Module / Contribution / Activation。
set(_gate_legacy_extension_bundle_regex
    "RuntimeFeatureHost|RuntimeFeaturePackage|RuntimeFeatureCompensation|RenderViewExtension|installViewExtension")
set(_gate_legacy_extension_bundle_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_extension_bundle_max    0)
set(_gate_legacy_extension_bundle_target 0)
set(_gate_legacy_extension_bundle_batch  "runtime extensions")
set(_gate_legacy_extension_bundle_fix
    "二进制只由 ExtensionModuleManager 管理；World/Render/Editor 分别使用\n    自己的 Catalog+Host。渲染扩展只走 RenderEffect + 可选 extraction batch。")

# ── G2f ── component schema 是 composition-injected main-thread catalog。
set(_gate_legacy_component_registry_regex  "ComponentTypeRegistry")
set(_gate_legacy_component_registry_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_component_registry_max    0)
set(_gate_legacy_component_registry_target 0)
set(_gate_legacy_component_registry_batch  "runtime extensions")
set(_gate_legacy_component_registry_fix
    "显式传递 ComponentTypeCatalog/WorldDocumentCodec；generated schema 先进 registrar\n    transaction。不得恢复 singleton、replace-on-duplicate 或转发头。")

# registry.ctx 只保留与 EnTT 信号及 registry 生命周期不可分割的层级索引。
# Scene service、持久身份、领域 runtime 和命令队列都必须由 composition 显式拥有。
set(_gate_registry_ctx_boundary_regex  "\\.ctx[ \t\r\n]*\\(")
set(_gate_registry_ctx_boundary_roots  "modules;ecs;engine;platforms")
set(_gate_registry_ctx_boundary_allowed_files
    "ecs/core/include/lux/engine/ecs/HierarchyIndex.hpp"
    "ecs/core/include/lux/engine/ecs/systems/HierarchicalTransformSystem.hpp"
    "ecs/core/src/Schedule.cpp")
set(_gate_registry_ctx_boundary_max    0)
set(_gate_registry_ctx_boundary_target 0)
set(_gate_registry_ctx_boundary_batch  "ECS-first scene ownership")
set(_gate_registry_ctx_boundary_fix
    "registry.ctx 只允许 HierarchyIndex 的信号寿命绑定。其余 owner 进入\n    SceneServices 或 composition 显式成员，并通过构造参数/SceneServiceRef 注入。")

# World Partition 候选只能从 Source 覆盖的宏区和稀疏索引产生。旧系统通过
# registry.view<Mesh...>() 扫描全场并挂休眠 tag，世界越大每帧成本越高；
# Active ECS 现在只包含已经激活的实体，不再用 tag 模拟卸载。
set(_gate_legacy_world_streaming_scan_regex
    "WorldStreamingSystem|RenderDormantComponent|RenderAssetEvictedComponent")
set(_gate_legacy_world_streaming_scan_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_world_streaming_scan_max    0)
set(_gate_legacy_world_streaming_scan_target 0)
set(_gate_legacy_world_streaming_scan_batch  "Spatial Partition")
set(_gate_legacy_world_streaming_scan_fix
    "流送选择器只向 SpatialPartition 提交 dimension-neutral demand；卸载实体\n    必须从 Registry 真正移除，不得恢复逐帧全场扫描或 Dormant/Evicted tag。")

# The former World runtime was a parallel fact bus beside EnTT. These owners
# and their dimension/domain switches have been physically removed; scene
# content now enters only through EntitySection publication and domain leaves
# observe their own ECS components.
set(_gate_legacy_world_runtime_bus_regex
    "runtime_world_partition|SceneWorldPartition|SceneWorldDomainControllers|PreparedWorldDomainSection|WorldRenderClusterRegistry|WorldTerrainRegistry|WorldEntityResolver")
set(_gate_legacy_world_runtime_bus_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_world_runtime_bus_max    0)
set(_gate_legacy_world_runtime_bus_target 0)
set(_gate_legacy_world_runtime_bus_batch  "ECS-first EntityScene")
set(_gate_legacy_world_runtime_bus_fix
    "运行期 scene 内容只经 EntitySection -> command barrier -> registry；领域\n    系统观察自己的 component/blob。不得恢复并行 World registry/controller。")

# LXSC/LXES are the only cooked scene entry kinds. Authoring source files may
# keep their project-facing extension, but the Pak and Player must never infer
# a scene identity from one of the retired LXWM/LXWS entry discriminators.
set(_gate_legacy_world_asset_types_regex
    "EAssetType::WORLD([^A-Za-z0-9_]|$)|EAssetType::WORLD_SECTION|EAssetType::WORLD_MANIFEST_PAGE|kWorldImageMagic|kWorldSectionImageMagic|kWorldManifestPageEntryMagic")
set(_gate_legacy_world_asset_types_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_world_asset_types_max    0)
set(_gate_legacy_world_asset_types_target 0)
set(_gate_legacy_world_asset_types_batch  "ECS-first EntityScene")
set(_gate_legacy_world_asset_types_fix
    "只发布显式 ENTITY_SCENE/LXSC 与 ENTITY_SECTION/LXES Pak entry；ID 由\n    cooked bundle 提供，不解析旧 World wire 固定偏移，也不保留 enum shim。")

# The old LXWM/LXWS codec and page/local coordinate representation are not an
# Authoring interchange format. Runtime content uses LXSC/LXES and ordinary
# double positions; reintroducing any of these types creates a second scene
# source or a second Transform representation.
set(_gate_legacy_world_wire_regex
    "WorldCodec|WorldManifest|WorldDomainPayload|WorldStreaming|WorldCoordinates|WorldPosition2D|WorldPosition3D|WorldLocal2f|WorldLocal3f|kDefaultCoordinatePageSize")
set(_gate_legacy_world_wire_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_world_wire_max    0)
set(_gate_legacy_world_wire_target 0)
set(_gate_legacy_world_wire_batch  "ECS-first EntityScene")
set(_gate_legacy_world_wire_fix
    "运行期场景只读 LXSC/LXES，空间事实只用普通 double Position；不得恢复\n    LXWM/LXWS codec、World domain payload 或 page/local Transform 双真相。")

# Editable World-source identity and partition layout belong to the Authoring
# product. Keeping them in Resource makes Player install an authoring-only
# model and leaves an easy edge for Runtime to depend on it again.
set(_gate_legacy_world_resource_model_regex
    "lux::world|lux/engine/resource/world/|resource::world|lux-engine-resource REQUIRED COMPONENTS world|WorldEntityId")
set(_gate_legacy_world_resource_model_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_world_resource_model_max    0)
set(_gate_legacy_world_resource_model_target 0)
set(_gate_legacy_world_resource_model_batch  "Authoring source ownership")
set(_gate_legacy_world_resource_model_fix
    "可编辑 World source 身份与 partition layout 只归 authoring_world；Resource\n    与 Player 不安装、不链接旧 world target，也不保留 namespace/include shim。")

set(_gate_legacy_world_cooker_regex
    "toolchain_world_cook|WorldCooker|world_cooker_test")
set(_gate_legacy_world_cooker_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_world_cooker_max    0)
set(_gate_legacy_world_cooker_target 0)
set(_gate_legacy_world_cooker_batch  "EntityScene Toolchain")
set(_gate_legacy_world_cooker_fix
    "Authoring 输入经领域叶 adapter 进入 toolchain_entity_scene_cook；不得恢复 LXWM/LXWS World cooker 或把旧 cooked wire 加回 Runtime。")

# LXSC contribution selection and the live contribution host are the only
# scene capability facts. A parallel dimension/activation plan makes Editor,
# Runtime and persistence disagree as soon as dependencies are selected or an
# activation changes at runtime.
set(_gate_legacy_scene_plan_regex
    "ScenePlanInfo|SceneLoadResult|SceneExtensionManifest|runtime_scene_io|EWorldDimension")
set(_gate_legacy_scene_plan_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_scene_plan_max    0)
set(_gate_legacy_scene_plan_target 0)
set(_gate_legacy_scene_plan_batch  "ECS-first EntityScene")
set(_gate_legacy_scene_plan_fix
    "场景能力只来自 LXSC contribution 与 live contribution host；不得恢复
    dimension/activation 第二份计划、旧 SceneLoadResult 或 scene/io shim。")

# 2D capability selection is contribution-based. Presentation, simulation and
# physics must remain independently selectable so headless physics does not
# acquire render dependencies.
set(_gate_legacy_scene2d_pack_regex
    "runtime_pack_scene2d|org\\.lux\\.builtin\\.scene2d|makeScene2DContribution|scene2DContributionId")
set(_gate_legacy_scene2d_pack_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_scene2d_pack_max    0)
set(_gate_legacy_scene2d_pack_target 0)
set(_gate_legacy_scene2d_pack_batch  "orthogonal Scene contributions")
set(_gate_legacy_scene2d_pack_fix
    "分别选择 spatial2d.transform/simulation2d/presentation2d/physics2d；不得恢复一次安装全部 2D 领域并把 headless physics 链到 render 的顶层 pack。")

# 3D capability selection is contribution-based. A single descriptor which
# installs presentation, physics and navigation recreates the deleted D3
# World type at the link/activation layer and makes headless closure impossible.
set(_gate_legacy_scene3d_pack_regex
    "runtime_pack_scene3d|org\\.lux\\.builtin\\.scene3d|makeScene3DContribution")
set(_gate_legacy_scene3d_pack_roots
    "modules;ecs;engine;extensions;platforms")
set(_gate_legacy_scene3d_pack_max    0)
set(_gate_legacy_scene3d_pack_target 0)
set(_gate_legacy_scene3d_pack_batch  "orthogonal Scene contributions")
set(_gate_legacy_scene3d_pack_fix
    "分别选择 animation3d/presentation3d/physics3d/navigation3d 与独立 spatial3d transform/partition；不得恢复一次安装所有 3D 领域的顶层 pack。")

# ── G2g ── main-thread completion 类型已按语义统一命名。
set(_gate_legacy_main_thread_names_regex
    "(^|[^A-Za-z0-9_])(MainQueue|MainHandle|MainScheduler)([^A-Za-z0-9_]|$)|drainMain[ \t\r\n]*\\(")
set(_gate_legacy_main_thread_names_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_main_thread_names_max    0)
set(_gate_legacy_main_thread_names_target 0)
set(_gate_legacy_main_thread_names_batch  "main-thread mailbox")
set(_gate_legacy_main_thread_names_fix
    "只使用 MainThreadMailbox/MainThreadDispatcher/MainThreadScheduler 与\n    drainMainThreadCompletions；不留旧名 alias/shim。")

# Target DAG catches link edges; these source gates also catch private include
# leaks and header coupling before a product profile reaches the linker.
set(_gate_modules_upper_layer_include_regex
    "#[ \t]*include[ \t]*[<\"]lux/engine/(ecs|runtime|authoring|toolchain|editor|hosts)/")
set(_gate_modules_upper_layer_include_roots  "modules")
set(_gate_modules_upper_layer_include_max    0)
set(_gate_modules_upper_layer_include_target 0)
set(_gate_modules_upper_layer_include_batch  "product DAG")
set(_gate_modules_upper_layer_include_fix
    "modules 只能依赖下层公共 API；领域适配移到 ECS 或 Runtime Integration。")

set(_gate_ecs_upper_layer_include_regex
    "#[ \t]*include[ \t]*[<\"]lux/engine/(runtime|authoring|toolchain|editor|hosts)/")
set(_gate_ecs_upper_layer_include_roots  "ecs")
set(_gate_ecs_upper_layer_include_max    0)
set(_gate_ecs_upper_layer_include_target 0)
set(_gate_ecs_upper_layer_include_batch  "product DAG")
set(_gate_ecs_upper_layer_include_fix
    "ECS 不认识 Runtime/Authoring/Toolchain/Editor/Host；跨层装配进入 Runtime pack。")

set(_gate_runtime_upper_layer_include_regex
    "#[ \t]*include[ \t]*[<\"]lux/engine/(authoring|toolchain|editor|hosts)/")
set(_gate_runtime_upper_layer_include_roots  "engine/runtime")
set(_gate_runtime_upper_layer_include_max    0)
set(_gate_runtime_upper_layer_include_target 0)
set(_gate_runtime_upper_layer_include_batch  "product DAG")
set(_gate_runtime_upper_layer_include_fix
    "Runtime 只消费 cooked 格式和下层领域 API；源工程、编译器、Editor UI 与 Host 不得反向渗入。")

set(_gate_authoring_upper_layer_include_regex
    "#[ \t]*include[ \t]*[<\"]lux/engine/(ecs|runtime|editor|hosts)/")
set(_gate_authoring_upper_layer_include_roots  "engine/authoring")
set(_gate_authoring_upper_layer_include_max    0)
set(_gate_authoring_upper_layer_include_target 0)
set(_gate_authoring_upper_layer_include_batch  "product DAG")
set(_gate_authoring_upper_layer_include_fix
    "Authoring 只描述源数据；不得拥有 World、AsyncRuntime、Editor UI 或 Host。")

# EntityScene 是“实体与组件内容”的通用载体，不是新名字的 World
# 总线。具体领域只能通过 component schema + opaque attachment 进入。
set(_gate_entity_scene_domain_leak_regex
    "Pixel|Tilemap|Terrain|NavMesh|Physics|RenderCluster|WorldPartition|EWorldSectionKind|EWorldStreamingDomain|resource/world|runtime/world")
set(_gate_entity_scene_domain_leak_roots
    "modules/resource/entity_scene;engine/runtime/entity_scene")
set(_gate_entity_scene_domain_leak_max    0)
set(_gate_entity_scene_domain_leak_target 0)
set(_gate_entity_scene_domain_leak_batch  "ECS-first EntityScene")
set(_gate_entity_scene_domain_leak_fix
    "EntityScene 只保留 schema/archetype/Section/demand/blob 契约；领域 payload
    归各自 ECS 模块，通过叶集成观察组件。")

# Render extraction 只读当前 registry 事实。它不应知道实体是固定、
# 空间流送或无限生成，也不应借稳定内容 ID 推断视觉过渡。
set(_gate_ecs_render_world_leak_regex
    "resource/world|runtime/world|WorldEntityId|WorldAnchor|WorldTransform|WorldCoordinateContext|EWorldSectionKind|EWorldStreamingDomain")
set(_gate_ecs_render_world_leak_roots  "ecs/render/include;ecs/render/src")
set(_gate_ecs_render_world_leak_max    0)
set(_gate_ecs_render_world_leak_target 0)
set(_gate_ecs_render_world_leak_batch  "ECS-first Render")
set(_gate_ecs_render_world_leak_fix
    "Render 只观察 Transform/ResolvedTransform 和显式 VisualTransition；
    流送身份、Section 与 partition 不得进入渲染域。")

# 帧内物理交互使用完整、带 version 的 EnTT handle。持久身份是
# 可选内容事实，不能成为创建刚体或归因 contact 的前置条件。
set(_gate_physics3d_world_identity_regex  "WorldEntityId|StableEntityId")
set(_gate_physics3d_world_identity_roots  "ecs/physics3d/include;ecs/physics3d/src")
set(_gate_physics3d_world_identity_max    0)
set(_gate_physics3d_world_identity_target 0)
set(_gate_physics3d_world_identity_batch  "ECS-first identity")
set(_gate_physics3d_world_identity_fix
    "Physics contact/body maps 使用完整 entt::entity；发布前验证 version
    与所需组件，不要强制每个实体携带持久 ID。")

# ── G2h ── 生命周期关闭只允许 sender-first 协议。固定泵送次数、同步
# scope close 和带 blocking 参数的帧提交都会重新制造关闭窗口竞态。
set(_gate_legacy_lifecycle_wait_regex
    "closeWithRetry|ForcedAfterPumpBudget|completeCloseAfterEmpty|AsyncScope[ \t]*::[ \t]*Progress|submitFrame[ \t\r\n]*\\([ \t\r\n]*bool")
set(_gate_legacy_lifecycle_wait_roots  "modules;ecs;engine;platforms")
set(_gate_legacy_lifecycle_wait_max    0)
set(_gate_legacy_lifecycle_wait_target 0)
set(_gate_legacy_lifecycle_wait_batch  "生命周期线性化")
set(_gate_legacy_lifecycle_wait_fix
    "owner 暴露 closeAsync sender；composition root 只通过 MainCloseDriver\n    推进安全点并在终态后 join。帧背压由 FrameCoordinator 观察 epoch 后重试。")

# ── G2i ── 生产代码不得靠睡眠/让步推进关闭或异步状态机。真实等待由
# atomic epoch、Asio completion 或 Vulkan timeline 表达。
set(_gate_shutdown_spin_wait_regex
    "std[ \t]*::[ \t]*this_thread[ \t]*::[ \t]*(sleep_for|yield)[ \t]*\\(")
set(_gate_shutdown_spin_wait_roots  "modules;ecs;engine;platforms")
set(_gate_shutdown_spin_wait_max    0)
set(_gate_shutdown_spin_wait_target 0)
set(_gate_shutdown_spin_wait_batch  "生命周期线性化")
set(_gate_shutdown_spin_wait_fix
    "把条件变成 generation-safe epoch/completion；关闭等待集中到\n    MainCloseDriver。不要用 1ms 轮询、yield 或固定次数重试掩盖协议缺口。")

# ── G2j ── Texture 像素只允许 SharedBytes 或显式 copyOf()。
set(_gate_raw_texture_ownership_regex
    "owns_data_|Texture[ \t\r\n]*\\([ \t\r\n]*(const[ \t]+)?void[ \t]*\\*")
set(_gate_raw_texture_ownership_roots  "modules/resource;ecs;engine;platforms")
set(_gate_raw_texture_ownership_max    0)
set(_gate_raw_texture_ownership_target 0)
set(_gate_raw_texture_ownership_batch  "不可变共享字节")
set(_gate_raw_texture_ownership_fix
    "Texture::fromShared 接收 SharedBytes；临时 view 必须显式走 copyOf。\n    不得恢复 void* + owns_data/copy 的分裂所有权。")

# ── G2k ── POD upload reply 在 coordinator 静态解码，不克隆通用 byte vector。
set(_gate_generic_upload_reply_clone_regex  "OwnedReply")
set(_gate_generic_upload_reply_clone_roots  "modules;ecs;engine;platforms")
set(_gate_generic_upload_reply_clone_max    0)
set(_gate_generic_upload_reply_clone_target 0)
set(_gate_generic_upload_reply_clone_batch  "typed upload reply")
set(_gate_generic_upload_reply_clone_fix
    "按静态 Reply 类型解码后把小值移动到 MainThreadMailbox；变长 readback 使用\n    自己的 owning result，不恢复通用 vector<byte> payload clone。")

# ── G2l ── 上传跨线程 API 不接受裸数据指针；借用临时 view 只能通过名字
# 明确的 Copy 路径立即取得 owner。
set(_gate_upload_raw_payload_regex
    "try(Create|Update)[A-Za-z0-9_]*[ \t\r\n]*\\([^\\)]*(void|std::byte|byte|std::uint8_t|uint8_t)[ \t]*\\*")
set(_gate_upload_raw_payload_roots  "modules/function/render/client/include/lux/engine/function/render/client")
set(_gate_upload_raw_payload_max    0)
set(_gate_upload_raw_payload_target 0)
set(_gate_upload_raw_payload_batch  "不可变共享字节")
set(_gate_upload_raw_payload_fix
    "跨线程上传接收 SharedBytes/owning mip/face/batch；临时 span 只允许\n    try*Copy 命名入口，并在返回前形成 owner。")

# ── G3 ── 业务层直接串 RenderRequest::then。**已达标**,从此只认文件
# 白名单,不再用「全仓允许 N 处」的计数天花板。旧门禁即使仍是 9/9,
# 也允许删掉一个旧续体、在任意业务节点新增一个——数字没变,架构却回归了。
#
# 合法边界只有:
#   · comm 原语内部(请求/租约实现与 ScopedRenderRequest 转发);
#   · runtime_render_scene 的私有 integration 把单个 RenderRequest 适配为 stdexec sender;
#   · OwnerReplyReaper 的回执落地主干(回执必须经 MainThreadScheduler，owner
#     关闭后只做 GPU 句柄补偿);
#   · ECS 的 TrackedRenderRequest(词法拥有 ScopedRenderRequest,续体只投递节点
#     私有完成记录,世界只在 update 安全点改)。
#
# 扫描同时覆盖 `.then(` 与 `->then(`;白名单之外一律是违规,所以终态是 0。
set(_gate_business_then_regex  "(\\.|->[ \t]*)then[ \t]*\\(")
set(_gate_business_then_roots
    "ecs;engine/runtime;engine/editor;engine/hosts;modules/function/render")
set(_gate_business_then_allowed_files
    "ecs/render/include/lux/engine/ecs/render/TrackedRenderRequest.hpp"
    "engine/runtime/render/scene/pinclude/lux/engine/runtime/render/scene/detail/RenderRequestSender.hpp"
    "engine/runtime/render/scene/pinclude/lux/engine/runtime/render/scene/detail/residency/OwnerReplyReaper.hpp"
    # RenderEffectHost is the render-control RPC -> sender integration seam;
    # domain workflows consume the typed ticket, never RenderRequest::then.
    "engine/runtime/extensions/contribution_host/pinclude/lux/engine/runtime/extensions/detail/RenderRequestSender.hpp"
    "modules/function/render/client/include/lux/engine/function/render/client/RenderRequest.hpp"
    "modules/function/render/client/src/RenderLease.cpp"
    "modules/function/render/client/src/RenderControlSession.cpp"
    "modules/function/render/client/src/RenderUploadClient.cpp"
    "modules/function/render/client/src/RenderUploadSession.cpp")
set(_gate_business_then_max    0)
set(_gate_business_then_target 0)
set(_gate_business_then_batch  "已达标(防回归)")
set(_gate_business_then_fix
    "业务层多步异步编排走 typed operation + stdexec sender + 明确\n    AsyncScope;持久 GPU 数据只经 coordinator-owned RenderUploadClient。\n    ECS 节点只持有 TrackedRenderRequest,在 update 安全点 drain;不得\n    在节点里直接 `.then`/`->then`。真正边界适配器须连同理由加入白名单。")

# ── G4 ── 业务层直接碰低层帧协议。
# 只认 session 限定的调用(`session_.beginFrame` / `ctx->session().submitFrame`),
# 脚本后端也有一个自己的 beginFrame,那是同名不同物,不该算进来。
# 终态只允许 composition root 借 FrameCoordinator 开闭正常帧。
set(_gate_frame_protocol_regex
    "ensureFrameOpen|[Ss]ession[a-zA-Z_]*(\\(\\))?[ \t]*(\\.|->)[ \t]*(beginFrame|submitFrame|pumpReplies)")
set(_gate_frame_protocol_roots
    "ecs;engine/editor;engine/hosts;engine/runtime;platforms")
# The one composition owner is deliberately the protocol boundary: while
# GeneralRenderServer drains already-accepted work during stop(), this host
# must pump frame/control replies so bounded response rings cannot stall the
# render thread. Upload replies belong exclusively to AsyncRuntime's
# coordinator. No scene/system/pack is allowed to pump any endpoint itself.
set(_gate_frame_protocol_allowed_files
    "engine/runtime/frame/src/FrameCoordinator.cpp"
    "engine/runtime/render/backend_host/include/lux/engine/runtime/render/backend_host/RenderBackendHost.hpp"
    "engine/runtime/render/scene/src/AsyncRenderUploadService.cpp")
set(_gate_frame_protocol_max    0)
set(_gate_frame_protocol_target 0)
set(_gate_frame_protocol_batch  "已达标(防回归)")
set(_gate_frame_protocol_fix
    "帧编排走 FrameCoordinator 的高层接口;system/node 不知道\n    帧是否开着,也不主动 pump 或 submit(设计稿 §3 不变量 3)。")

# ── G5 ── RTTI。当前生产源实测为 0,这一条是**防回归**,不是待办。
set(_gate_rtti_regex  "dynamic_cast[ \t]*<|typeid[ \t]*\\(")
set(_gate_rtti_roots  "modules;engine;ecs;platforms")
set(_gate_rtti_max    0)
set(_gate_rtti_target 0)
set(_gate_rtti_batch  "已达标(防回归)")
set(_gate_rtti_fix
    "类型身份统一用 lux::cxx::type_hash + type_name 的 TypeToken;\n    type-erased 槽保存生成的 destroy/move 函数指针,不做 RTTI downcast。")

# ── G6 ── 项目自有的 C++ 异常控制流。
# 分阶段迁:render/Vulkan 构造 API → serialize/resource/project → FlowForge。
set(_gate_exceptions_regex
    "(^|[^A-Za-z0-9_])throw[ \t]*(\\(|[^A-Za-z0-9_])|(^|[^A-Za-z0-9_])catch[ \t]*\\(")
set(_gate_exceptions_roots  "modules;engine;ecs;platforms")
set(_gate_exceptions_max    0)
set(_gate_exceptions_target 0)
set(_gate_exceptions_batch  "G")
set(_gate_exceptions_fix
    "可恢复失败返回 lux::cxx::expected<T,E>;重量级构造用 static factory +\n    expected,不用会抛的构造函数,也不用两阶段 init()。OOM 走进程级 fatal\n    policy,不在业务层假装可恢复。")

# ── 扫描 ──────────────────────────────────────────────────────────────────

set(_failed "")
set(_summary "")

foreach(_gate IN LISTS _gates)
    set(_regex  "${_gate_${_gate}_regex}")
    set(_roots  "${_gate_${_gate}_roots}")
    set(_max    "${_gate_${_gate}_max}")
    set(_target "${_gate_${_gate}_target}")

    set(_count 0)
    set(_detail "")

    foreach(_root IN LISTS _roots)
        file(GLOB_RECURSE _sources
             "${REPO_ROOT}/${_root}/*.cpp"
             "${REPO_ROOT}/${_root}/*.hpp"
             "${REPO_ROOT}/${_root}/*.h"
             "${REPO_ROOT}/${_root}/*.inl")
        foreach(_file IN LISTS _sources)
            if(_file MATCHES "${_exclude_regex}")
                continue()
            endif()
            file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_file}")
            string(REPLACE "\\" "/" _rel "${_rel}")

            # 某些机制只能出现在少数边界适配器中。对它们用**文件白名单**,
            # 而不是可在任意地方消费的全局计数配额。文件被搬走时白名单也必须
            # 显式更新,这使「新边界」无法伪装成「数量没变」混过门禁。
            set(_allowed_files "${_gate_${_gate}_allowed_files}")
            if(_allowed_files AND _rel IN_LIST _allowed_files)
                continue()
            endif()
            # ⚠️ 不用 file(STRINGS ... REGEX):它把结果拼成 CMake 列表,而**以反斜杠
            # 结尾的行**(宏续行)会转义掉分隔符,把后面若干行并成一项 —— 计数因此
            # 静默偏低(IR.cpp 的 4 处 throw 被数成 2)。整文件读进来跑 MATCHALL 没有
            # 这个问题:门禁表里的模式都是短记号,不含分号也不含反斜杠,不会再被
            # 列表语义咬一口。副作用是计的从「命中行数」变成「出现次数」——对棘轮
            # 而言后者更准(同一行写两次也该被看见)。
            file(READ "${_file}" _text)
            # ⚠️ 先剥注释,只数**代码**。
            #
            # 成因:批 B0 拆掉 `releaseAssetRefs` 的倒序遍历时,把「为什么这条约束
            # 不成立」写进了注释 —— 于是解释一个正在退场的机制,反而把它的计数顶过
            # 了上限。门禁因此在惩罚好注释,而本仓的规矩恰恰是「注释解释为什么」。
            # 剥掉注释之后,计数量的就是真实使用面,写多少解释都不影响。
            string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")
            string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
            string(REGEX MATCHALL "${_regex}" _hits "${_text}")
            list(LENGTH _hits _n)
            if(_n GREATER 0)
                math(EXPR _count "${_count} + ${_n}")
                string(APPEND _detail "    ${_rel}: ${_n}\n")
            endif()
        endforeach()
    endforeach()

    # 报告一律用 string(APPEND) 直接拼,不走 CMake 列表:门禁表的中文处方里有
    # 半角分号,当成列表元素拼接时它会被当成分隔符,渲染出来就是莫名其妙的断行。
    if(_count GREATER _max)
        string(APPEND _failed
             "  [${_gate}] ${_count} 处 > 上限 ${_max}（批 ${_gate_${_gate}_batch}，终态 ${_target}）\n${_detail}    处方：${_gate_${_gate}_fix}\n\n")
    elseif(_count LESS _max)
        message(WARNING
            "architecture_gates: [${_gate}] 已降到 ${_count} 处（上限还写着 ${_max}）——\n"
            "  把 cmake/architecture_gates_test.cmake 里的 _gate_${_gate}_max 改成 ${_count}，"
            "棘轮才咬得住。")
    endif()

    string(APPEND _summary
           "  ${_gate}: ${_count} / 上限 ${_max} / 终态 ${_target}（批 ${_gate_${_gate}_batch}）\n")
endforeach()

# ── Build profile / target platform orthogonality ──────────────────────────
#
# Product profiles describe installed closure and launch products.  Operating
# systems belong to the target-platform axis and therefore must never grow a
# second, OS-shaped profile.  Keep the old spelling out of all build logic;
# the root CMakeLists intentionally retains it only to produce a precise
# migration error for stale command lines.
string(CONCAT _legacy_platform_profile "ANDROID" "_PLAYER")
set(_profile_platform_failures "")
set(_profile_platform_files "")
foreach(_root IN ITEMS modules ecs engine extensions platforms bootstrap)
    file(GLOB_RECURSE _root_profile_platform_files
        "${REPO_ROOT}/${_root}/CMakeLists.txt"
        "${REPO_ROOT}/${_root}/*.cmake"
        "${REPO_ROOT}/${_root}/*.json")
    list(APPEND _profile_platform_files ${_root_profile_platform_files})
endforeach()
foreach(_file IN LISTS _profile_platform_files)
    if(_file MATCHES "${_exclude_regex}")
        continue()
    endif()
    file(READ "${_file}" _text)
    string(REGEX REPLACE "#[^\n]*" "" _text "${_text}")
    string(FIND "${_text}" "${_legacy_platform_profile}" _legacy_index)
    string(REGEX MATCH
        "LUX_PROFILE_HAS_PLAYER|LUX_BUILD_PROFILE[^\n\r]*(ANDROID|WINDOWS|LINUX|MACOS|IOS)"
        _axis_conflation
        "${_text}")
    if(NOT _legacy_index EQUAL -1 OR _axis_conflation)
        file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_file}")
        string(REPLACE "\\" "/" _rel "${_rel}")
        string(APPEND _profile_platform_failures "  ${_rel}\n")
    endif()
endforeach()
if(_profile_platform_failures)
    string(APPEND _failed
        "  [profile_platform_orthogonality] 产品配置与目标平台再次耦合。\n"
        "${_profile_platform_failures}    处方：Profile 只使用 DEVELOPER/PLAYER/EDITOR/TOOLCHAIN；"
        "平台条件只读取 LUX_TARGET_OS/LUX_TARGET_ARCH。\n\n")
endif()
string(APPEND _summary
    "  profile_platform_orthogonality: 产品配置与目标平台正交（防回归）\n")

# ── Typed feature operation lane completeness ──────────────────────────────
#
# `kind` describes the wire shape; `lane` decides which ownership/admission
# protocol is legal.  Leaving lane implicit would regenerate the old bug where
# a resource upload happened to travel inside a FrameProgram.  The generator
# also rejects a missing lane, while this source gate makes the reason visible
# without requiring meta generation to run first.
# A pure Toolchain product cooks concrete built-in ECS columns. Its source
# configure must therefore contain the exact schema/meta target subset instead
# of relying on a previously installed ECS SDK. Conversely, this must not turn
# Toolchain into Runtime or acquire Vulkan/UI/Lua backends.
set(_toolchain_ecs_profile_failures "")
macro(_lux_require_toolchain_profile_text relative_path required_text reason)
    file(READ "${REPO_ROOT}/${relative_path}" _profile_file_text)
    string(FIND "${_profile_file_text}" "${required_text}" _profile_text_index)
    if(_profile_text_index EQUAL -1)
        string(APPEND _toolchain_ecs_profile_failures
            "  ${relative_path}: ${reason}\n")
    endif()
endmacro()

_lux_require_toolchain_profile_text(
    "CMakeLists.txt"
    "if(LUX_PROFILE_HAS_RUNTIME OR LUX_PROFILE_HAS_TOOLCHAIN)\n    add_subdirectory(ecs)"
    "TOOLCHAIN 必须从当前源树配置 ECS")
_lux_require_toolchain_profile_text(
    "CMakeLists.txt"
    "set(LUX_BUILD_COMPONENT_LUA_META OFF)"
    "TOOLCHAIN 必须关闭 Lua meta 而保留普通 component meta")
_lux_require_toolchain_profile_text(
    "CMakeLists.txt"
    "if(LUX_PROFILE_HAS_RUNTIME OR LUX_PROFILE_HAS_TOOLCHAIN)\n    list(APPEND CPACK_COMPONENTS_ALL lux_engine_ecs)"
    "TOOLCHAIN 安装闭包必须包含 lux_engine_ecs")
_lux_require_toolchain_profile_text(
    "CMakeLists.txt"
    "lux-engine-authoring, lux-engine-ecs, lux-engine-resource, lux-engine-function"
    "lux-engine-toolchain 包必须声明 ECS 依赖")
_lux_require_toolchain_profile_text(
    "modules/function/CMakeLists.txt"
    "set(dir_list animation render/client)"
    "schema 闭包必须配置 backend-neutral animation/render_client")
_lux_require_toolchain_profile_text(
    "ecs/CMakeLists.txt"
    "if(LUX_BUILD_PROFILE STREQUAL \"TOOLCHAIN\")"
    "ECS 必须有显式 Toolchain schema 子集")
foreach(_required_ecs_component IN ITEMS
        core spatial3d_components terrain_components physics3d navigation3d
        render_components render_components_3d)
    _lux_require_toolchain_profile_text(
        "ecs/CMakeLists.txt"
        "        ${_required_ecs_component}\n"
        "Toolchain ECS 安装子集缺 ${_required_ecs_component}")
endforeach()
_lux_require_toolchain_profile_text(
    "ecs/render/CMakeLists.txt"
    "if(NOT LUX_BUILD_PROFILE STREQUAL \"TOOLCHAIN\")"
    "Toolchain 只配置 render component/schema，不配置 extraction 行为")

if(_toolchain_ecs_profile_failures)
    string(APPEND _failed
        "  [toolchain_ecs_profile_closure] Toolchain ECS schema/meta 闭包不完整。\n"
        "${_toolchain_ecs_profile_failures}    处方：从当前源树配置并打包明确的 ECS schema/meta 依赖；"
        "不得隐式 find_package 已安装 SDK，也不得把 Runtime/Vulkan/UI/Lua 带入 Toolchain。\n\n")
endif()
string(APPEND _summary
    "  toolchain_ecs_profile_closure: Toolchain 显式拥有 schema/meta 闭包（防回归）\n")

file(GLOB_RECURSE _operation_headers
     "${REPO_ROOT}/modules/function/render/client/include/*Operation.hpp")
set(_lane_failures "")
set(_lane_op_total 0)
foreach(_file IN LISTS _operation_headers)
    file(READ "${_file}" _text)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")
    string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
    string(REGEX MATCHALL "LUX_OP[ \t\r\n]*\\(" _ops "${_text}")
    string(REGEX MATCHALL "(^|[^A-Za-z0-9_])lane[ \t\r\n]*=" _lanes "${_text}")
    list(LENGTH _ops _op_count)
    list(LENGTH _lanes _lane_count)
    math(EXPR _lane_op_total "${_lane_op_total} + ${_op_count}")
    if(NOT _op_count EQUAL _lane_count)
        file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_file}")
        string(APPEND _lane_failures
            "  ${_rel}: LUX_OP=${_op_count}, lane=${_lane_count}\n")
    endif()
endforeach()
if(_lane_failures)
    string(APPEND _failed
        "  [operation_lane] 每个 LUX_OP 必须且只能声明一个 lane。\n"
        "${_lane_failures}    处方：按语义显式选择 frame/control/upload；"
        "不得从 kind 推断 lane。\n\n")
endif()
string(APPEND _summary
    "  operation_lane: ${_lane_op_total} 个 op 均有显式 lane（防回归）\n")

# Control/upload packets are single operations, never disguised frames.
set(_operation_lane_files
    "modules/function/render/client/include/lux/engine/function/render/client/RenderControlSession.hpp"
    "modules/function/render/client/include/lux/engine/function/render/client/RenderUploadSession.hpp"
    "modules/function/render/client/src/RenderControlSession.cpp"
    "modules/function/render/client/src/RenderUploadSession.cpp")
set(_operation_lane_failures "")
foreach(_rel IN LISTS _operation_lane_files)
    file(READ "${REPO_ROOT}/${_rel}" _text)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")
    string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
    string(REGEX MATCHALL "FrameProgram|beginFrame|submitFrame" _hits "${_text}")
    list(LENGTH _hits _count)
    if(_count GREATER 0)
        string(APPEND _operation_lane_failures "  ${_rel}: ${_count}\n")
    endif()
endforeach()
if(_operation_lane_failures)
    string(APPEND _failed
        "  [operation_packet_only] control/upload 不得依赖帧协议。\n"
        "${_operation_lane_failures}    处方：使用 OperationPacket，生命周期由各自 channel 管理。\n\n")
endif()
string(APPEND _summary
    "  operation_packet_only: control/upload 与 FrameProgram 解耦（防回归）\n")

# Runtime assertions disappear under NDEBUG and cannot enforce wire safety.
set(_comm_assert_failures "")
file(GLOB_RECURSE _comm_sources
    "${REPO_ROOT}/modules/function/render/client/include/lux/engine/function/render/client/*.hpp"
    "${REPO_ROOT}/modules/function/render/client/src/*.cpp")
foreach(_file IN LISTS _comm_sources)
    file(READ "${_file}" _text)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")
    string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
    string(REGEX REPLACE "static_assert" "" _text "${_text}")
    string(REGEX MATCHALL "(^|[^A-Za-z0-9_])assert[ 	\r\n]*\\(" _hits "${_text}")
    list(LENGTH _hits _count)
    if(_count GREATER 0)
        file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_file}")
        string(APPEND _comm_assert_failures "  ${_rel}: ${_count}\n")
    endif()
endforeach()
if(_comm_assert_failures)
    string(APPEND _failed
        "  [comm_runtime_assert] render comm 生产代码不得依赖 assert。\n"
        "${_comm_assert_failures}    处方：协议输入返回 RenderError；不可恢复的本地不变量走 renderFatal。\n\n")
endif()
string(APPEND _summary
    "  comm_runtime_assert: render comm 无运行期 assert（防回归）\n")

# Standalone Asio is the single completion backend. Check both C++ sources and
# build descriptions: source-only scans would miss a reintroduced Boost link.
file(GLOB_RECURSE _asio_boundary_files
    "${REPO_ROOT}/engine/*.cpp"
    "${REPO_ROOT}/engine/*.hpp"
    "${REPO_ROOT}/ecs/*.cpp"
    "${REPO_ROOT}/ecs/*.hpp"
    "${REPO_ROOT}/modules/*.cpp"
    "${REPO_ROOT}/modules/*.hpp"
    "${REPO_ROOT}/platforms/*.cpp"
    "${REPO_ROOT}/platforms/*.hpp"
    "${REPO_ROOT}/*/CMakeLists.txt"
    "${REPO_ROOT}/*.cmake"
    "${REPO_ROOT}/cmake/*.cmake")
set(_asio_boundary_failures "")
foreach(_file IN LISTS _asio_boundary_files)
    if(_file MATCHES "${_exclude_regex}")
        continue()
    endif()
    file(READ "${_file}" _text)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")
    string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
    string(REGEX MATCHALL
        "boost[ \\t\\r\\n]*::[ \\t\\r\\n]*asio|Boost[ \\t\\r\\n]*::|exec[ \\t\\r\\n]*::[ \\t\\r\\n]*asio[ \\t\\r\\n]*::[ \\t\\r\\n]*use_sender"
        _hits "${_text}")
    list(LENGTH _hits _count)
    if(_count GREATER 0)
        file(RELATIVE_PATH _rel "${REPO_ROOT}" "${_file}")
        string(APPEND _asio_boundary_failures "  ${_rel}: ${_count}\n")
    endif()
endforeach()
if(_asio_boundary_failures)
    string(APPEND _failed
        "  [standalone_asio] 不得引入 Boost.Asio 或上游 exception-based use_sender。\n"
        "${_asio_boundary_failures}    处方：链接 standalone asio::asio；使用本仓 error_code → expected sender 适配器。\n\n")
endif()

set(_execution_cmake "${REPO_ROOT}/engine/runtime/execution/CMakeLists.txt")
file(READ "${_execution_cmake}" _execution_cmake_text)
set(_missing_asio_definitions "")
foreach(_definition IN ITEMS
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
    ASIO_NO_TYPEID
    ASIO_NO_TS_EXECUTORS
    ASIO_NO_EXCEPTIONS)
    if(NOT _execution_cmake_text MATCHES "${_definition}")
        string(APPEND _missing_asio_definitions "  ${_definition}\n")
    endif()
endforeach()
if(_missing_asio_definitions)
    string(APPEND _failed
        "  [asio_compile_contract] execution 缺少必须的 standalone/no-exception 定义。\n"
        "${_missing_asio_definitions}\n")
endif()
string(APPEND _summary
    "  standalone_asio: standalone/no-exception 边界完整（防回归）\n")

# ── Product/source boundaries ─────────────────────────────────────────────
#
# Target DAG validation catches link edges, but it cannot see a header-only
# dependency or a forward include that happens to compile because another
# target leaked the include directory.  These source gates make the public
# product boundaries independently enforceable.
macro(_lux_check_source_boundary boundary roots regex prescription)
    set(_boundary_failures "")
    foreach(_boundary_root IN LISTS roots)
        file(GLOB_RECURSE _boundary_files
            "${REPO_ROOT}/${_boundary_root}/*.cpp"
            "${REPO_ROOT}/${_boundary_root}/*.hpp"
            "${REPO_ROOT}/${_boundary_root}/*.h"
            "${REPO_ROOT}/${_boundary_root}/CMakeLists.txt")
        foreach(_boundary_file IN LISTS _boundary_files)
            if(_boundary_file MATCHES "${_exclude_regex}")
                continue()
            endif()
            file(READ "${_boundary_file}" _boundary_text)
            string(REGEX REPLACE
                "/\\*([^*]|\\*+[^*/])*\\*+/"
                ""
                _boundary_text
                "${_boundary_text}")
            string(REGEX REPLACE "//[^\n]*" "" _boundary_text "${_boundary_text}")
            string(REGEX MATCHALL "${regex}" _boundary_hits "${_boundary_text}")
            list(LENGTH _boundary_hits _boundary_count)
            if(_boundary_count GREATER 0)
                file(RELATIVE_PATH
                    _boundary_rel
                    "${REPO_ROOT}"
                    "${_boundary_file}")
                string(REPLACE "\\" "/" _boundary_rel "${_boundary_rel}")
                string(APPEND
                    _boundary_failures
                    "  ${_boundary_rel}: ${_boundary_count}\n")
            endif()
        endforeach()
    endforeach()
    if(_boundary_failures)
        string(APPEND _failed
            "  [${boundary}] source/include 产品边界被穿透。\n"
            "${_boundary_failures}    处方：${prescription}\n\n")
    endif()
    string(APPEND _summary
        "  ${boundary}: source/include 边界为零（防回归）\n")
endmacro()

set(_execution_boundary_roots "engine/runtime/execution")
_lux_check_source_boundary(
    execution_domain_blind
    _execution_boundary_roots
    "lux/engine/(resource/asset|function/render|ecs|editor|authoring|toolchain)|lux::engine::(resource::asset|function::render|ecs|editor|authoring|toolchain)|lux::(asset|render|ecs|editor|authoring|toolchain)::"
    "AsyncRuntime 只保留 typed operation、scheduler、Asio/TBB/BlockingIO 与 MainThreadMailbox；资产、渲染、ECS、Editor 必须作为上层 client。")

set(_legacy_public_surface_roots
    "modules;ecs;engine;extensions;platforms")
_lux_check_source_boundary(
    legacy_asset_public_surface
    _legacy_public_surface_roots
    "lux/engine/asset/|lux::engine::resource::asset([^_A-Za-z0-9]|$)"
    "Asset 公共头只能使用 lux/engine/resource/asset/ 新路径，CMake 依赖必须在 asset_identity/core/codecs/pak 中按语义选择；禁止恢复旧头或聚合 target。")

set(_runtime_product_boundary_roots "engine/runtime")
_lux_check_source_boundary(
    runtime_product_clean
    _runtime_product_boundary_roots
    "lux/engine/(authoring|toolchain|editor)/|lux::engine::(authoring|toolchain|editor)|lux::(authoring|toolchain)::"
    "Runtime 只能消费 cooked resource/function/ECS/runtime API；源文档、compiler/importer 与 Editor workflow 必须留在上层产品。")

set(_authoring_boundary_roots "engine/authoring")
_lux_check_source_boundary(
    authoring_runtime_blind
    _authoring_boundary_roots
    "lux/engine/(runtime|ecs|editor)/|lux::engine::(runtime|ecs|editor)|lux::(runtime|ecs|editor)::"
    "Authoring 只描述源数据，可由 Editor/Toolchain 共同读取；不得执行 World、GPU、AsyncRuntime 或 UI orchestration。")

set(_player_boundary_roots
    "engine/hosts/player;engine/hosts/game_application")
_lux_check_source_boundary(
    player_product_clean
    _player_boundary_roots
    "lux/engine/(authoring|toolchain|editor)/|lux::engine::(authoring|toolchain|editor)|lux::(authoring|toolchain|editor)::"
    "Player Host 只做 cooked Runtime composition；项目源文件、导入/编译工具和 Editor 类型不得进入其源码或 link interface。")

set(_runtime_authored_document_roots
    "modules/resource;modules/function;ecs;engine/runtime;engine/hosts/player;engine/hosts/game_application")
_lux_check_source_boundary(
    runtime_authored_document_clean
    _runtime_authored_document_roots
    "lux/engine/authoring/(assets/material|flowforge)/|lux/engine/toolchain/(asset/material|shader)/|FlowGraphScript|FlowGraphRef|class[ \t]+MaterialGraph([^A-Za-z0-9_]|$)|class[ \t]+FlowGraph([^A-Za-z0-9_]|$)|class[ \t]+NodeRegistry([^A-Za-z0-9_]|$)"
    "Runtime/Player 只见 cooked MaterialData、Script 与 shader bytecode contract；MaterialGraph、FlowGraph、NodeRegistry 和 lowering/compiler 必须留在 Authoring/Toolchain。")

set(_game_application_boundary_roots "engine/hosts/game_application")
_lux_check_source_boundary(
    game_application_platform_neutral
    _game_application_boundary_roots
    "android_native_app_glue|android/(asset_manager|log|native_window)|windows\.h|GlfwRuntime|LuxWindow"
    "game_application 只接收 opaque native surface、尺寸和平台唤醒；Win32/GLFW/Android 生命周期必须留在薄平台适配器。")

set(_android_game_adapter_roots "platforms/android/game")
_lux_check_source_boundary(
    android_game_adapter_thin
    _android_game_adapter_roots
    "lux/engine/runtime/(frame|scene/SceneRuntime|scene/script|render/(scene|backend_host)|execution|assets|extensions)/|lux/engine/ecs/(World|ComponentTypeCatalog|render/)"
    "Android game 入口只做 APK、NativeWindow、ALooper 与日志适配；Runtime/Scene/Render/Extension/关闭协议统一委托 game_application。")

set(_render_frontend_boundary_roots
    "modules/function/render/client;modules/function/render/graph")
_lux_check_source_boundary(
    render_frontend_backend_neutral
    _render_frontend_boundary_roots
    "#[ \t]*include[ \t]*[<\"]vulkan/|find_package[ \t\r\n]*\\([ \t\r\n]*Vulkan|Vulkan::"
    "render_client 与 logical render_graph 必须能在没有 Vulkan SDK 时独立编译；Vk 资源、barrier 与 submit 只进入 render_vulkan。")

set(_spatial_partition_core_roots
    "engine/runtime/spatial_partition/include;engine/runtime/spatial_partition/src")
_lux_check_source_boundary(
    spatial_partition_domain_blind
    _spatial_partition_core_roots
    "#[ \\t]*include[ \\t]*[<\"]lux/engine/[^\"]*(pixel|tilemap|terrain|navigation|physics|render|spatial2d|spatial3d)[^\"]*[>\"]|lux::(pixel|tilemap|terrain|navigation|physics|render)::|lux::(ecs|resource|runtime)::[A-Za-z0-9_:]*(Pixel|Tilemap|Terrain|Navigation|Physics|Render)[A-Za-z0-9_:]*"
    "SpatialPartition 只处理 Section record、demand、ticket、generation 与预算；2D/3D 选择器和 Pixel/Tilemap/Terrain/Navigation/Physics/Render 领域必须留在叶 target。")

set(_entity_section_leaf_roots
    "engine/runtime/packs;engine/runtime/spatial2d;engine/runtime/spatial3d;engine/runtime/spatial_partition")
_lux_check_source_boundary(
    entity_section_narrow_clients
    _entity_section_leaf_roots
    "findService[ \t\r\n]*<[ \t\r\n]*[^>]*EntitySectionLoaderSystem|typeToken[ \t\r\n]*<[ \t\r\n]*[^>]*EntitySectionLoaderSystem"
    "领域 contribution 与空间 selector 只借用 EntitySectionClient/ContentBlobClient；具体 loader 的状态机与 owner 生命周期只属于 SceneRuntime。")

set(_infinite2d_boundary_roots "engine/runtime/spatial2d")
_lux_check_source_boundary(
    infinite2d_dimension_clean
    _infinite2d_boundary_roots
    "#[ \\t]*include[ \\t]*[<\"]lux/engine/(resource/world|runtime/world|[^\"]*3d[^\"]*)/|lux::engine::(world|resource::world|runtime::world)|lux::engine::(ecs|resource|runtime)::[A-Za-z0-9_:]*3d[A-Za-z0-9_:]*|lux::(world|terrain|navigation)::|lux_engine_[A-Za-z0-9_]*3d"
    "Infinite2D 只依赖 EntityScene、二维空间适配、dimension-neutral partition 与明确的 2D ECS 领域；三维 World/Terrain/Navigation/Render 只能在独立叶 pack 中出现。")

set(_infinite2d_production_roots
    "engine/runtime/spatial2d/infinite/include;engine/runtime/spatial2d/infinite/src")
_lux_check_source_boundary(
    infinite2d_pixel_tilemap_open_closed
    _infinite2d_production_roots
    "[Tt]ilemap"
    "Tilemap 作为第二种二维内容只能新增自己的组件/provider/consumer；Infinite2D/Pixel production 叶不得感知或分派 Tilemap。")

set(_tilemap2d_leaf_roots
    "engine/runtime/spatial2d/tilemap/include;engine/runtime/spatial2d/tilemap/src")
_lux_check_source_boundary(
    tilemap2d_domain_leaf
    _tilemap2d_leaf_roots
    "#[ \\t]*include[ \\t]*[<\"]lux/engine/[^\"]*(render|pixel|physics|navigation|spatial3d|spatial_partition|spatial2d/infinite)[^\"]*[>\"]|lux::(render|physics|navigation)::|SpatialInterest2DSystem"
    "Tilemap 内容叶只解码 LXTC、解析 PersistentEntityRef 并维护自己的 runtime；渲染、Pixel、物理、导航与空间选择通过独立 participant/adapter 组合。")

if(_failed)
    message(FATAL_ERROR
        "架构门禁：有机制的使用量**上升**了。\n\n${_failed}"
        "这些是重构中正在退场的机制（设计稿 "
        ".internal/ecs-render-execution-lifetime-refactor.zh-CN.md）。\n"
        "新代码不该再用它们。确有理由提高上限，请连同理由一起改门禁表。\n\n"
        "当前全表：\n${_summary}")
endif()

message(STATUS "architecture_gates: 全部机制未超上限\n${_summary}")
