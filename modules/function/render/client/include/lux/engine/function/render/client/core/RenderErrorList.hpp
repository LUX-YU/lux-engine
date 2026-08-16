#pragma once
/**
 * @file RenderErrorList.hpp
 * @brief 引擎自带的错误类型声明,以及驱动批量登记 / 重名检查的 X-macro 列表。
 *
 * 新增一个引擎错误类型:
 *   1. 在下面对应的 err::<组> 命名空间里写一个空结构体,四个 static constexpr 成员;
 *   2. 在 LUX_RENDER_ERROR_LIST 里加一行 X(...)。
 * 两步之外没有别的登记动作 —— 构造 RenderErrorRegistry 时整张表一次登记完,重名在
 * 编译期由 static_assert 拦住。
 *
 * `name` 是**跨会话稳定的契约**:测试断言、门禁规则、上层分诊逻辑一律按 name 写。
 * ErrorTypeId 只是它在本次会话里的句柄,不要持久化。
 *
 * 实参槽的取舍原则:**只有当该类型的每一个失败点都真的拿得到那个数,才声明它**。
 * 声明了却在某处填 0,消息会理直气壮地报出一个假事实(比如把「没拿到 VkResult」
 * 渲染成 VK_SUCCESS),比不带实参更糟。所以「创建返回空句柄」与「调用返回了错误码」
 * 是两个类型,而不是一个类型的两种填法。
 *
 * feature 私有的错误类型不进这张表 —— 它们在 feature 的 register_ops_fn 里
 * errorType<T>()、在 unregister_ops_fn 里 forget<T>(),与自己的 op id 同生命周期。
 */

#include <lux/engine/function/render/client/core/RenderError.hpp>

namespace lux::render::err
{
    // ── 设备与 Vulkan 对象 ────────────────────────────────────────────────
    namespace device
    {
        /// 创建动作交回了空句柄。绝大多数创建路径是这样报错的 —— 它们检查句柄而不是
        /// 保留 VkResult,所以这个类型不带实参。
        struct VulkanObjectCreationFailed
        {
            static constexpr const char* name     = "device.vulkan_object_creation_failed";
            static constexpr const char* message  = "Vulkan 对象创建失败,拿到空句柄";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        /// 一次 Vulkan 调用返回了非 VK_SUCCESS,且调用点保留了那个码。
        struct VulkanCallFailed
        {
            static constexpr const char* name     = "device.vulkan_call_failed";
            static constexpr const char* message  = "Vulkan 调用失败:{0}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::VkResult};
        };

        /// A generic VkResult promoted at the frame lifecycle boundary. The
        /// phase distinguishes begin, end/submit and surface retirement when
        /// a terminal failure cannot be delivered through the response ring.
        struct FrameLifecycleCallFailed
        {
            static constexpr const char* name =
                "device.frame_lifecycle_call_failed";
            static constexpr const char* message =
                "帧生命周期阶段 {0} 的 Vulkan 调用失败:{1}";
            static constexpr ERecovery recovery = ERecovery::Permanent;
            static constexpr ErrorArgs args{
                EErrorArg::Uint,
                EErrorArg::VkResult};
        };

        /// The surface is temporarily unable to provide the data required by
        /// one swapchain build stage (minimised, destroyed, or changing).
        struct SwapchainUnavailable
        {
            static constexpr const char* name     = "device.swapchain_unavailable";
            static constexpr const char* message  = "交换链阶段 {0} 暂不可用";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// An explicitly recoverable WSI result. Keep both the build stage and
        /// exact signed VkResult bit pattern; neither may be inferred later.
        struct SwapchainSurfaceChanged
        {
            static constexpr const char* name     = "device.swapchain_surface_changed";
            static constexpr const char* message  = "交换链阶段 {0} 返回可重建错误:{1}";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::VkResult};
        };

        struct SwapchainVulkanCallFailed
        {
            static constexpr const char* name     = "device.swapchain_vulkan_call_failed";
            static constexpr const char* message  = "交换链阶段 {0} 的 Vulkan 调用失败:{1}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::VkResult};
        };

        struct SwapchainConfigurationInvalid
        {
            static constexpr const char* name     = "device.swapchain_configuration_invalid";
            static constexpr const char* message  = "交换链阶段 {0} 的本地配置无效";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct SwapchainBuildContractViolated
        {
            static constexpr const char* name     = "device.swapchain_build_contract_violated";
            static constexpr const char* message  = "交换链阶段 {0} 成功却未产生有效对象";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct RenderPassCreationFailed
        {
            static constexpr const char* name     = "device.render_pass_creation_failed";
            static constexpr const char* message  = "render pass 创建失败:{0}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::VkResult};
        };

        /// 帧环容量是设备会话的构造参数。超出固定上限不能靠取模掩盖:那会让
        /// 两个逻辑在飞帧静默复用同一组 fence / command buffer。
        struct InvalidFramesInFlight
        {
            static constexpr const char* name     = "device.invalid_frames_in_flight";
            static constexpr const char* message  = "在飞帧数 {0} 超出支持范围 [1, {1}]";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };
    } // namespace device

    // ── 着色器 ───────────────────────────────────────────────────────────
    namespace shader
    {
        /// 内置着色器解析不出模块:嵌入表里没有这一项,或它的反射反序列化失败。
        /// 这不是 I/O —— SPIR-V 是编译期嵌进二进制的。
        struct BuiltinUnavailable
        {
            static constexpr const char* name     = "shader.builtin_unavailable";
            static constexpr const char* message  = "内置着色器 {0} 解析不出模块";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::BuiltinShader};
        };

        /// 句柄查不到记录:已释放、代次过期,或从未注册。
        struct HandleStale
        {
            static constexpr const char* name     = "shader.handle_stale";
            static constexpr const char* message  = "着色器句柄查不到记录(已释放或代次过期)";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        /// 取不到源 SPIR-V 字节:该槽位不是经去重 add() 建的(预先构造的 ShaderObject
        /// 没有源字节),因此无法在它之上打补丁。
        struct SourceBytesUnavailable
        {
            static constexpr const char* name     = "shader.source_bytes_unavailable";
            static constexpr const char* message  = "该着色器槽位没有源 SPIR-V 字节,无法打补丁";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        struct SpirvParseFailed
        {
            static constexpr const char* name     = "shader.spirv_parse_failed";
            static constexpr const char* message  = "SPIR-V 解析失败,不能产出半打补丁的模块";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };

        /// 某个 set 只会被搬走一半:按名字逐 binding 搬运,而这个 set 里只有部分
        /// binding 登记进了契约。搬完的模块会同时引用新旧两个 set,而 Vulkan 不报错。
        struct SetPartiallyRelocatable
        {
            static constexpr const char* name     = "shader.set_partially_relocatable";
            static constexpr const char* message  =
                "set {0} 会被搬走一半 —— binding {1} 不在契约里,先补齐契约再切这条管线";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 反射只是模块的子集:SPIR-V 里的描述符变量比反射登记的多,按反射搬运会留下
        /// 残余引用,而布局是按反射建的,于是那条残余引用的 set 根本不存在。
        struct ReflectionIsSubsetOfModule
        {
            static constexpr const char* name     = "shader.reflection_is_subset_of_module";
            static constexpr const char* message  =
                "SPIR-V 里有 {0} 个描述符变量,反射只登记了 {1} 个(元数据漏报,需重烘焙)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 反射与 SPIR-V 字节不同源:该搬的变量数与实际搬动数不符。补丁改的是缓存字节,
        /// 搬运表算自反射,两者一旦漂移,Vulkan 不会报错,binding 只是接错。
        struct ReflectionOutOfSyncWithSpirv
        {
            static constexpr const char* name     = "shader.reflection_out_of_sync_with_spirv";
            static constexpr const char* message  =
                "应搬 {0} 个变量,实际搬了 {1} 个 —— 反射与 SPIR-V 字节不同源";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };
    } // namespace shader

    // ── 显存与容量 ───────────────────────────────────────────────────────
    namespace memory
    {
        struct GpuAllocationFailed
        {
            static constexpr const char* name     = "memory.gpu_allocation_failed";
            static constexpr const char* message  = "GPU 资源分配失败";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{};
        };

        struct OutOfMemory
        {
            static constexpr const char* name     = "memory.out_of_memory";
            static constexpr const char* message  = "内存不足";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{};
        };

        /// 定容池 / 竞技场 / 槽位表满了。
        struct CapacityExhausted
        {
            static constexpr const char* name     = "memory.capacity_exhausted";
            static constexpr const char* message  = "容量耗尽,无法再分配槽位";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{};
        };
    } // namespace memory

    // ── 资产(客户端送进来的数据) ────────────────────────────────────────
    namespace asset
    {
        struct Invalid
        {
            static constexpr const char* name     = "asset.invalid";
            static constexpr const char* message  = "资产数据为空或已损坏";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };

        struct UnsupportedFormat
        {
            static constexpr const char* name     = "asset.unsupported_format";
            static constexpr const char* message  = "不支持的像素格式";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };
    } // namespace asset

    // ── 引擎资源槽位与句柄 ───────────────────────────────────────────────
    namespace resource
    {
        struct NotFound
        {
            static constexpr const char* name     = "resource.not_found";
            static constexpr const char* message  = "句柄指向的资源不存在或已释放";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        struct AlreadyExists
        {
            static constexpr const char* name     = "resource.already_exists";
            static constexpr const char* message  = "该资源已存在,不能重复创建";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        struct TypeMismatch
        {
            static constexpr const char* name     = "resource.type_mismatch";
            static constexpr const char* message  = "修改请求的类型与资源实际类型不符";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };

        struct UnsupportedType
        {
            static constexpr const char* name     = "resource.unsupported_type";
            static constexpr const char* message  = "不支持的资源类型";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };

        struct ModifyFailed
        {
            static constexpr const char* name     = "resource.modify_failed";
            static constexpr const char* message  = "就地修改失败";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };
    } // namespace resource

    // ── 管线布局构建 ─────────────────────────────────────────────────────
    //
    // 域模式下,槽位号与每个 binding 的位置都是搬运表(名字 → 契约 → 域内偏移)的
    // 确定性函数。SPIR-V、反射、布局三者必须指向同一个位置;任一方落后都在注册时
    // 拒绝,而不是留到录制期以 VUID 的形式冒出来。
    //
    // 这些错误的实参里只带**实际值**与契约索引 —— 期望值(该住哪个槽、该在哪个
    // binding)是契约的纯函数,消费侧拿着索引自己就能算出来。
    namespace pipeline
    {
        /// 各 stage 的域合并状态不一致:一部分 shader 已搬运、另一部分还带着会移动的
        /// 引擎资源没搬。两侧的位置对不上,而 Vulkan 不会报错。
        struct StageMergeStateConflict
        {
            static constexpr const char* name     = "pipeline.stage_merge_state_conflict";
            static constexpr const char* message  =
                "各 stage 的域合并状态不一致 —— 切换管线必须让全部 stage 都过同一个切换入口";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        /// 契约资源出现在了不属于它的域槽位上。
        struct DomainSlotMismatch
        {
            static constexpr const char* name     = "pipeline.domain_slot_mismatch";
            static constexpr const char* message  = "{0} 属 {1} 域,却出现在槽 {2}";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{
                EErrorArg::LogicalResource, EErrorArg::BindFrequency, EErrorArg::Uint};
        };

        /// 域模式管线的某个 binding 不在契约里。半搬门禁本该在着色器侧就拦住,走到这里
        /// 说明反射与模块不同源。
        struct BindingNotInContract
        {
            static constexpr const char* name     = "pipeline.binding_not_in_contract";
            static constexpr const char* message  =
                "set {0} 的 binding {1} 不在契约里(反射与模块不同源?)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 反射里的位置与契约推出的域位置不符。
        struct BindingPositionMismatch
        {
            static constexpr const char* name     = "pipeline.binding_position_mismatch";
            static constexpr const char* message  =
                "{0} 的反射位置是 (set {1}, binding {2}),与契约推出的域位置不符";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{
                EErrorArg::LogicalResource, EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 反射里的描述符类型与引擎形状表不符。
        struct BindingTypeMismatch
        {
            static constexpr const char* name     = "pipeline.binding_type_mismatch";
            static constexpr const char* message  = "{0} 的描述符类型与引擎形状表不一致";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::LogicalResource};
        };

        /// 该域的共享布局还没建好。
        struct DomainLayoutNotInitialised
        {
            static constexpr const char* name     = "pipeline.domain_layout_not_initialised";
            static constexpr const char* message  = "{0} 域的共享布局尚未初始化";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::BindFrequency};
        };

        /// 管线引用了引擎契约资源却没带域合并标记 —— 说明有 stage 没过切换入口。
        /// 旧的 per-set 兜底已退休,这里不再回落:兜底的代价是静默,错配要等到录制期才
        /// 以 VUID-00358 的形式暴露,那时已经离现场很远。
        struct ContractResourceNotMerged
        {
            static constexpr const char* name     = "pipeline.contract_resource_not_merged";
            static constexpr const char* message  =
                "set {1} 引用契约资源 {0},但本管线未标记域合并";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::LogicalResource, EErrorArg::Uint};
        };

        /// 注册成功了,但某个 set 的反射布局取不出来 —— 着色器没有声明这个 set,或者
        /// 反射与模板声明的 set 数不一致。调用方通常需要它来分配 transient 描述符集。
        struct ReflectedSetLayoutMissing
        {
            static constexpr const char* name     = "pipeline.reflected_set_layout_missing";
            static constexpr const char* message  = "管线的 set {0} 没有反射布局";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 同一个模板的着色器变体预算已满。
        struct VariantBudgetExhausted
        {
            static constexpr const char* name     = "pipeline.variant_budget_exhausted";
            static constexpr const char* message  = "模板 {0} 的变体预算已满(上限 {1}),本次退回无特性变体";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// vkCreateGraphicsPipelines 失败。不缓存失败结果 —— 下次图编译会重试
        /// (常见成因是驱动 OOM,或热重载窗口里一个坏掉的着色器模块)。
        struct GraphicsCreationFailed
        {
            static constexpr const char* name     = "pipeline.graphics_creation_failed";
            static constexpr const char* message  = "图形管线创建失败(模板 {0},特性位 {1}),下次编译重试";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Hex};
        };
    } // namespace pipeline

    // ── 描述符写目标 ─────────────────────────────────────────────────────
    namespace descriptor
    {
        /// 接下的域写目标为空(或全是 NULL 句柄)。域集是描述符的唯一写目标,空目标
        /// 意味着这个资源的描述符**一条都不会被写入** —— 而绑定一个从未写过的描述符集
        /// 是未定义行为,画面表现却可能只是「有些东西不见了」,没有任何一层会报错。
        ///
        /// 上游成因通常是场景的域描述符 set 分配失败(池尺寸不足 / 域布局缺失)。
        struct DomainWriteTargetEmpty
        {
            static constexpr const char* name     = "descriptor.domain_write_target_empty";
            static constexpr const char* message  =
                "域写目标为空 —— 该资源的描述符一条都不会被写入(域 set 分配失败?)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };
    } // namespace descriptor

    // ── render graph ─────────────────────────────────────────────────────
    //
    // 这一族的实参带的是**图内下标**而不是名字。图资源与 pass 的名字是用户自起的
    // (某个特性 create/import 时给的),既不是契约条目也没有稳定编号,装不进 32 位
    // 实参槽;而下标是可解析的 —— 客户端用 `DumpRenderGraph` 拿到这张图的文本转储,
    // 按下标查回名字。句柄指向的是一个客户端本来就能取到的东西。
    namespace graph
    {
        struct CompiledGraphInvalid
        {
            static constexpr const char* name     = "graph.compiled_graph_invalid";
            static constexpr const char* message  = "编译后的 render graph 无效";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        // ── 编译中止(compile_error) ────────────────────────────────────

        struct DependencyCycle
        {
            static constexpr const char* name     = "graph.dependency_cycle";
            static constexpr const char* message  = "render graph 存在依赖环";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        /// 导入资源缺少取值回调:纹理要 image_getter 或 slot,缓冲要 buffer_getter。
        struct ImportedResourceIncomplete
        {
            static constexpr const char* name     = "graph.imported_resource_incomplete";
            static constexpr const char* message  = "导入资源 {0} 缺少取值回调(纹理需 image_getter 或 slot,缓冲需 buffer_getter)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource};
        };

        /// 有 pass 按名字引用了一个本图里没人创建/导入的资源。多半是某个特性没装上,
        /// 或者它的扩展位没开。
        struct ReferencedResourceHasNoProducer
        {
            static constexpr const char* name     = "graph.referenced_resource_has_no_producer";
            static constexpr const char* message  = "被引用的 {0} 在本图里没有生产者(检查特性是否装上、扩展位是否开启)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource};
        };

        /// 一条 pass 因为**输入拿不到**被停掉(它读的资源没人生产)。
        ///
        /// 与 ReferencedResourceHasNoProducer 的分工:那条说的是"这个资源没人产",
        /// 是资源侧的事实;这条说的是"因此这条 pass 不跑了",是后果。一次未解析的
        /// 可选引用会先报前者、再对每个受影响的 pass 报一条后者 —— 因为"少了哪张图"
        /// 和"于是哪些效果没了"是使用者要分别知道的两件事。
        struct PassStarvedOfInput
        {
            static constexpr const char* name     = "graph.pass_starved_of_input";
            static constexpr const char* message  = "pass {0} 的输入 {1} 没有生产者,已停用该 pass(其效果本帧缺失)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::GraphResource};
        };

        struct ComputePassMissingPipeline
        {
            static constexpr const char* name     = "graph.compute_pass_missing_pipeline";
            static constexpr const char* message  = "计算 {0} 没有设置 compute_pipeline_handle";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        /// pass 声明了 kernel id，但注册表中没有 emitter，且它也没有 recorder
        /// fallback。继续执行会得到一条“成功录制但没有任何命令”的静默空 pass。
        struct KernelEmitterMissing
        {
            static constexpr const char* name     = "graph.kernel_emitter_missing";
            static constexpr const char* message  = "{0} 声明了 kernel，但没有 emitter/recorder，pass 不会录制任何命令";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        struct ComputePassPipelineStale
        {
            static constexpr const char* name     = "graph.compute_pass_pipeline_stale";
            static constexpr const char* message  = "计算 {0} 的 compute_pipeline_handle 已失效";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        /// 资源在一个队列上写、另一个队列上读,需要跨队列所有权转移 —— 尚未支持。
        struct CrossQueueTransferRequired
        {
            static constexpr const char* name     = "graph.cross_queue_transfer_required";
            static constexpr const char* message  = "{0} 需要跨队列所有权转移(写在队列 {1},读在队列 {2}),尚未支持";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource, EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 条件 pass 静态地拥有某资源的 CLEAR —— 条件不成立时这次清除不会发生,
        /// 后续读取的是未定义内容。
        struct ConditionalPassOwnsClear
        {
            static constexpr const char* name     = "graph.conditional_pass_owns_clear";
            static constexpr const char* message  = "条件 {0} 静态拥有 {1} 的 CLEAR —— 条件不成立时清除不发生";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::GraphResource};
        };

        struct ConditionalPassWritesUnconditionalRead
        {
            static constexpr const char* name     = "graph.conditional_pass_writes_unconditional_read";
            static constexpr const char* message  = "条件非图形 {0} 写 {1},但有无条件的读方";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::GraphResource};
        };

        /// 一个 render-pass 组里所有成员都是条件 pass,且没有构成条件链
        /// (setCondition(cond, tag))—— 无法静态决定这一组是否要开。
        struct AllConditionalPassGroup
        {
            static constexpr const char* name     = "graph.all_conditional_pass_group";
            static constexpr const char* message  = "render-pass 组的成员全是条件 pass 且未构成条件链,首个越界成员是 {0}";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        // ── RenderPassPlanner 的编译中止 ────────────────────────────────

        struct AttachmentFormatUnknown
        {
            static constexpr const char* name     = "graph.attachment_format_unknown";
            static constexpr const char* message  = "{0} 的附件 {1} 格式未定义(VK_FORMAT_UNDEFINED)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::GraphResource};
        };

        struct AttachmentPlanInvalid
        {
            static constexpr const char* name     = "graph.attachment_plan_invalid";
            static constexpr const char* message  = "{0} 的附件计划不合法(第 {1} 项)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::Uint};
        };

        // ── 布局计划告警(layout_plan->warnings)─────────────────────────
        //
        // 这一组是**软信号**:它们描述的问题(变体管线布局不兼容、域槽解析不到)
        // 会让某个绑定被跳过,但整张图仍然编得出来。硬转成编译失败会让任何真实
        // 场景图都出不来,所以保持告警;它们的价值在于门禁能捕获,以及诊断面板
        // 能显示 —— 而不是像以前那样只在 stderr 上闪一下。

        struct DomainSlotWithoutDomainSets
        {
            static constexpr const char* name     = "graph.domain_slot_without_domain_sets";
            static constexpr const char* message  = "{0} 的逻辑绑定解析到合并域槽,但本图没有域 set 实例 —— 跳过该槽";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        struct EngineSetUnresolved
        {
            static constexpr const char* name     = "graph.engine_set_unresolved";
            static constexpr const char* message  = "{0} 的 useEngineSet 未解析到域槽(逻辑集不属 FEATURE/GLOBAL,或本图无域实例)—— 跳过该槽";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass};
        };

        struct SharedSetBindingTypeConflict
        {
            static constexpr const char* name     = "graph.shared_set_binding_type_conflict";
            static constexpr const char* message  = "共享该 set 的多条管线里 set {0} binding {1} 的类型不一致";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct VariantLayoutIncompatible
        {
            static constexpr const char* name     = "graph.variant_layout_incompatible";
            static constexpr const char* message  = "{0}:变体管线与主管线在 set {1} 的布局来源不同 —— 按 pass 绑定的描述符 set 对该变体不兼容";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::Uint};
        };

        struct VariantSetCountMismatch
        {
            static constexpr const char* name     = "graph.variant_set_count_mismatch";
            static constexpr const char* message  = "{0}:变体管线的 set 数量({2})与主管线({1})不同";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::Uint, EErrorArg::Uint};
        };

        struct DomainBindingRangeOverlap
        {
            static constexpr const char* name     = "graph.domain_binding_range_overlap";
            static constexpr const char* message  = "域内 binding 区间重叠:set {0} 与 set {1} 都覆盖 binding {2}";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 合并后的管线仍占超过 4 套 set —— 私有搬迁没收敛(查 privateSetRemapFor)。
        struct MergedLayoutExceedsSetBudget
        {
            static constexpr const char* name     = "graph.merged_layout_exceeds_set_budget";
            static constexpr const char* message  = "合并管线占 {0} 套 set(上限 4)—— 私有搬迁没收敛";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 域合并**投影**后仍需超过 4 套 set。
        ///
        /// 与上面那条的分工:`MergedLayoutExceedsSetBudget` 测的是「**已经**切到
        /// 域合并布局的管线**实际**占了几套」——那是已经坏了;这条测的是「任意
        /// 管线按域合并**会需要**几套」,因此覆盖**尚未切换**的管线,是「还剩多少
        /// 活」的信号。两条都留着,因为它们在不同时间点失败。
        ///
        /// 阈值取 Vulkan 对 maxBoundDescriptorSets 的**最低保证**(4),不是本机
        /// 上限 —— 桌面显卡报 32,拿本机比等于这条永远不触发,而手机在第 5 套就
        /// 拒绝。全图只发一条(取最宽的那条管线);逐管线名单在 LayoutPlan 的
        /// `over_budget_strict` 里,随 DumpRenderGraph 走。
        struct ProjectedLayoutExceedsSetBudget
        {
            static constexpr const char* name     = "graph.projected_layout_exceeds_set_budget";
            static constexpr const char* message  = "pass {0} 域合并后仍需 {1} 套 set(Vulkan 最低保证 4)—— 保守口径:feature 自有 set 未并入";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::Uint};
        };

        /// 手写的 slot 语义与从反射推导出来的对不上。两侧各有一条(手写有/推导有)。
        struct AuthoredDerivedSlotMismatch
        {
            static constexpr const char* name     = "graph.authored_derived_slot_mismatch";
            static constexpr const char* message  = "slot 语义 {0} → 槽 {1} 在手写与推导两侧不一致(方向 {2}:0=手写有推导无,1=推导有手写无)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint, EErrorArg::Uint};
        };

        // ── 用途并集(RenderGraphCompiler)───────────────────────────────

        /// 创建者声明了 keep_transient(承诺仅作附件使用),却有 pass 以非附件角色
        /// 消费它。按创建者主权丢弃越界需求 —— 那个 pass 将无法合法访问本资源。
        struct TransientUsageViolation
        {
            static constexpr const char* name     = "graph.transient_usage_violation";
            static constexpr const char* message  = "{0} 声明 keep_transient,但有 pass 以非附件角色消费它(越界用途位 {1}),按创建者主权丢弃";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource, EErrorArg::Hex};
        };

        /// 创建者声明的用途位不足,由消费角色补齐。开火 = 抓到一处欠声明。
        struct UsageUnderdeclared
        {
            static constexpr const char* name     = "graph.usage_underdeclared";
            static constexpr const char* message  = "{0} 的用途位由消费角色补齐({1})—— 创建者声明不足";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource, EErrorArg::Hex};
        };

        /// transient descriptor 的类型和从资源角色推导的 image layout 不相容。
        struct TransientDescriptorLayoutMismatch
        {
            static constexpr const char* name     = "graph.transient_descriptor_layout_mismatch";
            static constexpr const char* message  = "{0} 的 transient descriptor 类型 {1} 与 image layout {2} 不相容";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphResource, EErrorArg::Uint, EErrorArg::Uint};
        };

        /// Resource descriptor binding 没有关联 declared_consume，图编译器因而无法
        /// 为这次外部读取建立排序边。
        struct ResourceBindingConsumeMissing
        {
            static constexpr const char* name     = "graph.resource_binding_consume_missing";
            static constexpr const char* message  = "{0} 的 descriptor slot {1} 没有 declared_consume，依赖对 render graph 不可见";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::GraphPass, EErrorArg::Uint};
        };

        // ── 图模板编译前置 ──────────────────────────────────────────────

        /// 目标槽的 VkFormat 不在格式映射表内,推不出 RG 纹理格式。**必须中止编译**:
        /// 回落到一个「合法但错误」的格式会绕过 RenderPassPlanner 的格式门,把错格式
        /// 烙进 VkPipeline(dynamic rendering 下格式失配没有任何诊断)。
        struct SlotFormatUnmapped
        {
            static constexpr const char* name     = "graph.slot_format_unmapped";
            static constexpr const char* message  = "目标槽 {0} 的 VkFormat({1})不在格式映射表内,无法推导 RG 纹理格式";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::DescriptorSlot, EErrorArg::VkFormat};
        };
    } // namespace graph

    // ── feature 装配 ─────────────────────────────────────────────────────
    namespace feature
    {
        /// feature 依赖的某个资源初始化失败。缺了它这个 feature 无法有意义地工作,
        /// 所以装配整体失败 —— 装上一个不工作的 feature 只会让问题在别处以「东西
        /// 不见了」的形式出现。
        struct ResourceInitFailed
        {
            static constexpr const char* name     = "feature.resource_init_failed";
            static constexpr const char* message  = "feature 依赖的资源初始化失败";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        struct FactoryHasNoCreateFn
        {
            static constexpr const char* name     = "feature.factory_has_no_create_fn";
            static constexpr const char* message  = "feature 工厂没有 create_fn";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        struct TypeIdCollision
        {
            static constexpr const char* name     = "feature.type_id_collision";
            static constexpr const char* message  = "feature 稳定类型 id {0} 已被另一个工厂占用";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 两个**不同类型**的 feature 用了同一个 `FeatureFactory.name`。
        ///
        /// 为什么这是 Bug 级而不是"重名而已":名字被当**类型断言**用过 ——
        /// DeferredLightingFeature 曾按 `f->name() == "ShadowMap"` 认兄弟特性,然后
        /// 无检查 `static_cast`。那两处已经改成按稳定 type id 认人,但按名字索引的地方
        /// 还在(comm 的 FeatureCatalog 按 name 存 op-id),撞名 = 拿到别人的 op-id。
        /// 而 `name` 按文档就是**插件自选的字符串**,唯一能拦住它的地方就是注册。
        /// 实参是**已占用该名字的那个类型**的 id 低 32 位,便于消费侧指认是谁。
        struct FeatureNameCollision
        {
            static constexpr const char* name     = "feature.name_collision";
            static constexpr const char* message  = "feature 名字已被类型 {0} 占用 —— 名字被当类型判据用,重名不可接受";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType};
        };

        /// AddFeature 指名的类型 id 没有对应的工厂 —— 客户端漏了 RegisterFeatureType,
        /// 或者拿的是另一个 server 实例上的 id。
        struct TypeNotRegistered
        {
            static constexpr const char* name     = "feature.type_not_registered";
            static constexpr const char* message  = "feature 类型 id {0} 未注册";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// UnregisterFeatureType reached the final registration while one or more
        /// scene instances still use its factory/module code.
        struct FeatureTypeInUse
        {
            static constexpr const char* name     = "feature.type_in_use";
            static constexpr const char* message  = "feature 类型 id {0} 仍有 {1} 个活实例";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        // ── 装载校验(RenderScene::beginInstall 的四道闸) ──────────────────
        //
        // 这四条判的都是**特性之间的声明式关系**,数据来自 FeatureDescriptor。
        // 实参槽带的是对方的类型(截断的 FeatureTypeId,见 EErrorArg::FeatureType)
        // —— 只说"有冲突"不说"和谁",调用方还得自己去猜。

        struct AlreadyInstalled
        {
            static constexpr const char* name     = "feature.already_installed";
            static constexpr const char* message  = "{0} 声明了 SinglePerScene,该场景已有一个实例";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType};
        };

        struct ConflictsWithInstalled
        {
            static constexpr const char* name     = "feature.conflicts_with_installed";
            static constexpr const char* message  = "{0} 与场景中已装的 {1} 声明冲突";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType, EErrorArg::FeatureType};
        };

        struct DependencyMissing
        {
            static constexpr const char* name     = "feature.dependency_missing";
            static constexpr const char* message  = "{0} 需要 {1},但它尚未装入该场景(先装它)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType, EErrorArg::FeatureType};
        };

        /// 特性声明了分级档案,但没有一行匹配本会话解析出的 EFeatureLevel。
        struct LevelProfileMissing
        {
            static constexpr const char* name     = "feature.level_profile_missing";
            static constexpr const char* message  = "{0} 没有为当前特性等级({1})声明档案";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType, EErrorArg::Uint};
        };

        /// 匹配到了档案行,但设备没有启用该行白名单里的能力。
        struct LevelRequirementsUnmet
        {
            static constexpr const char* name     = "feature.level_requirements_unmet";
            static constexpr const char* message  = "{0} 在当前等级要求的设备能力({1})未全部启用";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType, EErrorArg::Hex};
        };

        /// 卸载被反向依赖挡住:还有别的特性声明依赖它。
        struct StillRequiredByAnother
        {
            static constexpr const char* name     = "feature.still_required_by_another";
            static constexpr const char* message  = "{0} 仍被 {1} 依赖,先卸载依赖方";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType, EErrorArg::FeatureType};
        };

        struct RuntimeDisableUnsupported
        {
            static constexpr const char* name     = "feature.runtime_disable_unsupported";
            static constexpr const char* message  = "{0} 声明了 supports_runtime_disable=false,不能在运行期关闭";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::FeatureType};
        };

        /// 句柄不指向本场景的任何活特性(从未装入,或已卸载后槽位换代)。
        struct HandleStale
        {
            static constexpr const char* name     = "feature.handle_stale";
            static constexpr const char* message  = "feature 句柄已失效(槽 {0},代 {1})";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };
    } // namespace feature

    // ── 场景 ─────────────────────────────────────────────────────────────
    namespace scene
    {
        struct NotFound
        {
            static constexpr const char* name     = "scene.not_found";
            static constexpr const char* message  = "场景 id {0} 不存在(已销毁或从未创建)";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// removeView 的幂等守卫拒绝:句柄陈旧(从未存在/已被 GC 回收),或视图
        /// 已在销毁中(重复摘)。目标状态通常已经达成,对调用方无害 —— 但重复摘
        /// 往往说明所有权账本在两处各记了一份,值得知道。
        struct ViewNotRemovable
        {
            static constexpr const char* name     = "scene.view_not_removable";
            static constexpr const char* message  = "视图 {0} 不可摘除(句柄陈旧或已在销毁中)";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct SpatialOriginRebaseRejected
        {
            static constexpr const char* name =
                "scene.spatial_origin_rebase_rejected";
            static constexpr const char* message =
                "场景空间原点重建会让仍存活的渲染对象超出 int32 页差范围";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{};
        };
    } // namespace scene

    // ── Vulkan 校验层 ────────────────────────────────────────────────────
    namespace validation
    {
        /// 校验层报告的一条问题。**只在开启校验层时产生**,走自发上报 ——
        /// 它不是某次调用的返回值,而是驱动层在任意时刻回调过来的。
        ///
        /// 消息文本不随事件过线(实参槽只装数字)。带的是严重级别与消息哈希:
        /// 同一条消息在一批内因此能正确合并计数,而完整文本由校验层自己的
        /// 输出机制承载 —— 渲染库不替上层决定它出现在哪。
        struct LayerReport
        {
            static constexpr const char* name     = "validation.layer_report";
            static constexpr const char* message  = "Vulkan 校验层报告(严重级别 {0},消息指纹 {1})";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Hex};
        };

        /// 校验层消息在跨线程传输环里被丢弃的条数(环满,或生产者线程数超过
        /// 通道数)。丢掉的那些没有各自的类型与实参可言,只能报一个总数 ——
        /// 但报出来,好过让"校验层安静"与"消息被吞了"看起来一模一样。
        struct EventsDropped
        {
            static constexpr const char* name     = "validation.events_dropped";
            static constexpr const char* message  = "校验层消息因传输环溢出丢弃 {0} 条";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };
    } // namespace validation

    // ── comm 载荷契约 ────────────────────────────────────────────────────
    //
    // 客户端与渲染线程隔着一条二进制通道,双方各自编译。载荷长度、版本号、扩展位
    // 三样东西一旦对不上,后面读到的每一个字段都是错位的 —— 所以这几类失败必须在
    // 拷贝之前拦住,而且必须把「期望值 / 实际值」这一对数原样交给客户端:只说
    // 「版本不对」的话,对方还得自己去猜是哪一边旧了。
    namespace upload
    {
        struct StateTransitionInvalid
        {
            static constexpr const char* name =
                "upload.state_transition_invalid";
            static constexpr const char* message =
                "GPU 上传请求 {0} 的状态转换无效:当前 {1},目标 {2}";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{
                EErrorArg::Uint,
                EErrorArg::Uint,
                EErrorArg::Uint,
            };
        };

        struct ResultIdentityMismatch
        {
            static constexpr const char* name =
                "upload.result_identity_mismatch";
            static constexpr const char* message =
                "GPU 上传请求 {0} 的迟到/重复结果被拒绝(资源 {1},代次 {2})";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{
                EErrorArg::Uint,
                EErrorArg::Uint,
                EErrorArg::Uint,
            };
        };
    } // namespace upload

    namespace comm
    {
        struct RequestInvalid
        {
            static constexpr const char* name = "comm.request_invalid";
            static constexpr const char* message = "RenderRequest 没有关联的请求状态";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{};
        };

        struct RequestNotReady
        {
            static constexpr const char* name = "comm.request_not_ready";
            static constexpr const char* message = "RenderRequest 尚未完成";
            static constexpr ERecovery recovery = ERecovery::Retryable;
            static constexpr ErrorArgs args{};
        };

        struct ChannelStopping
        {
            static constexpr const char* name = "comm.channel_stopping";
            static constexpr const char* message = "渲染通信端点正在关闭";
            static constexpr ERecovery recovery = ERecovery::Permanent;
            static constexpr ErrorArgs args{};
        };

        struct PayloadSizeMismatch
        {
            static constexpr const char* name     = "comm.payload_size_mismatch";
            static constexpr const char* message  = "comm 载荷长度不符:期望 {0} 字节,实收 {1} 字节";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct ReplyTypeMismatch
        {
            static constexpr const char* name = "comm.reply_type_mismatch";
            static constexpr const char* message =
                "回复类型不符:期望 TypeId {0},实收 TypeId {1}";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct ConfigVersionMismatch
        {
            static constexpr const char* name     = "comm.config_version_mismatch";
            static constexpr const char* message  = "comm config 版本不符:期望 {0},实收 {1}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct DescriptorLayoutVersionMismatch
        {
            static constexpr const char* name     = "comm.descriptor_layout_version_mismatch";
            static constexpr const char* message  = "descriptor layout 版本不符:期望 {0},实收 {1}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 扩展位是「客户端要求引擎打开的可选行为」。认不出的位不能忽略 —— 忽略等于
        /// 装了一个少了某项行为的 feature,而客户端以为它开着。
        struct UnknownExtensionFlags
        {
            static constexpr const char* name     = "comm.unknown_extension_flags";
            static constexpr const char* message  = "comm 载荷带有本引擎不认识的扩展位 {0}";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Hex};
        };

        /// 命令指名了第 {0} 号附件,但这一帧只带了 {1} 个。客户端侧的 addFeature 一定
        /// 会 emplace 一个附件,所以这说明帧被截断或载荷被改写过 —— 不是「没配置」。
        struct AttachmentIndexOutOfRange
        {
            static constexpr const char* name     = "comm.attachment_index_out_of_range";
            static constexpr const char* message  = "附件下标 {0} 越界(本帧共 {1} 个附件)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct AttachmentTypeMismatch
        {
            static constexpr const char* name     = "comm.attachment_type_mismatch";
            static constexpr const char* message  = "附件类型不符:期望 {0},实收 {1}";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        // ── 帧分发的四道闸(FrameDispatcher::execute) ────────────────────
        //
        // 这四条判的是**帧本身是否可解**,发生时整帧中止。它们都不是"业务失败",
        // 而是"这帧字节不是我们约定的那种字节":命令域越界、载荷偏移出界、
        // TypeId 不在注册表里、TypeId 的槽位已被回收换代。
        //
        // 数字必须带上:一条"分发失败"对排查毫无用处,而 opcode / 偏移 / 下标
        // 恰好就是定位到具体那条命令所需的全部信息。

        struct InvalidOpcode
        {
            static constexpr const char* name     = "comm.invalid_opcode";
            static constexpr const char* message  = "命令域 opcode={0} 越界";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct PayloadOutOfBounds
        {
            static constexpr const char* name     = "comm.payload_out_of_bounds";
            static constexpr const char* message  = "载荷区间越界:偏移 {0} + 长度 {1} 超出本帧载荷";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct UnknownTypeId
        {
            static constexpr const char* name     = "comm.unknown_type_id";
            static constexpr const char* message  = "TypeId 下标 {0} 未注册处理器";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// TypeId 的槽位被回收后重新分配给了别的类型(代数不匹配)。命令是拿着
        /// 一份过期的 id 表发出的 —— 照着执行会派给完全不相干的处理器。
        struct TypeIdGenerationMismatch
        {
            static constexpr const char* name     = "comm.type_id_generation_mismatch";
            static constexpr const char* message  = "TypeId 下标 {0} 的代数不匹配(槽位已被回收重分配)";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 该 (opcode, TypeId) 槽登记过,但函数指针是空的 —— 注册期漏填。
        struct HandlerNotRegistered
        {
            static constexpr const char* name     = "comm.handler_not_registered";
            static constexpr const char* message  = "TypeId 下标 {0} 的处理器函数指针为空";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// Client-side feature binding did not resolve the requested operation.
        /// Returning an unresolved request here used to wedge every caller's
        /// pending state forever.
        struct FeatureOperationUnavailable
        {
            static constexpr const char* name =
                "comm.feature_operation_unavailable";
            static constexpr const char* message =
                "请求的 feature operation 没有已注册的 TypeId";
            static constexpr ERecovery recovery = ERecovery::Bug;
            static constexpr ErrorArgs args{};
        };

        /// 批量载荷的字节数不是元素大小的整数倍 —— 两侧编译的元素类型不一致。
        struct BulkPayloadNotMultiple
        {
            static constexpr const char* name     = "comm.bulk_payload_not_multiple";
            static constexpr const char* message  = "批量载荷 {1} 字节不是元素大小 {0} 的整数倍";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct BulkPayloadMisaligned
        {
            static constexpr const char* name     = "comm.bulk_payload_misaligned";
            static constexpr const char* message  = "批量载荷起始地址不满足 {0} 字节对齐";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };
    } // namespace comm

    // ── 延迟光照的 G-buffer 读路径 ───────────────────────────────────────
    namespace lighting
    {
        /// 客户端显式要求 INPUT_ATTACHMENT(local-read 合并作用域),但设备没有启用
        /// KHR_dynamic_rendering_local_read。**这里不降级到 SAMPLED** —— 两条路径的
        /// tile 显存流量差一个量级,替客户端换一条是替它做了它没授权的决策。
        /// 客户端应当先 QueryDeviceCaps,再决定要哪条。
        struct LocalReadUnsupported
        {
            static constexpr const char* name     = "lighting.local_read_unsupported";
            static constexpr const char* message  = "设备未启用 KHR_dynamic_rendering_local_read,无法满足 INPUT_ATTACHMENT 读模式";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{};
        };

        struct ReadModeInvalid
        {
            static constexpr const char* name     = "lighting.read_mode_invalid";
            static constexpr const char* message  = "ELightingReadMode 取值 {0} 不在有效范围内";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// G-buffer 的 LocalReadScope 扩展位与光照的 INPUT_ATTACHMENT 读模式必须同开
        /// 同关:前者决定 G-buffer 写进合并作用域,后者决定光照用 subpassLoad() 读。
        /// 一边开一边关会产出一条读不到附件的管线,而且要到录制期才炸。
        struct ReadModeScopeMismatch
        {
            static constexpr const char* name     = "lighting.read_mode_scope_mismatch";
            static constexpr const char* message  = "G-buffer LocalReadScope={0} 与光照 INPUT_ATTACHMENT={1} 不一致";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 光照的 SPIR-V 变体按 technique 选定(PCF 采 D32 图集,EVSM 采 RGBA16F),
        /// 与场景里 ShadowMap 的活动 technique 错配就会采错图集 —— 画面无声出错,
        /// 没有任何 validation 层会报。两个 CommConfig 必须写同一个值。
        struct ShadowTechniqueMismatch
        {
            static constexpr const char* name     = "lighting.shadow_technique_mismatch";
            static constexpr const char* message  = "光照 technique={0} 与 ShadowMap 活动 technique={1} 不一致(会采错阴影图集)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };
    } // namespace lighting

    // ── 每帧资源仲裁 ─────────────────────────────────────────────────────
    //
    // 这一组**不走返回值,走自发上报**(RenderContext::reportError)。共同点是
    // 发生时一帧已经在飞了,没有调用方可以交待 —— 「请求 200 个阴影,图集只放得下
    // 30 个」不是任何「报错」能解决的问题,动态资源仲裁本来就是这些子系统的正当
    // 职责。它们此前的真问题不是「该报错却没报」,而是**完全不可见**。
    namespace frame
    {
        struct SkinningInputPoolUnavailable
        {
            static constexpr const char* name     = "frame.skinning_input_pool_unavailable";
            static constexpr const char* message  = "蒙皮输入顶点池不可用,本帧跳过该实例";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{};
        };

        struct BonePaletteFull
        {
            static constexpr const char* name     = "frame.bone_palette_full";
            static constexpr const char* message  = "本帧骨骼调色板已满(该实例需要 {0} 根),跳过该实例";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct SkinnedVertexPoolExhausted
        {
            static constexpr const char* name     = "frame.skinned_vertex_pool_exhausted";
            static constexpr const char* message  = "蒙皮输出顶点池耗尽(需要 {0} 顶点),跳过该实例";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        struct SkinningDispatchParamsOverflow
        {
            static constexpr const char* name     = "frame.skinning_dispatch_params_overflow";
            static constexpr const char* message  = "蒙皮 dispatch 参数 SSBO 容量不足(需要 {0},上限 {1}),跳过本批";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint};
        };

        struct VertexPoolRegistryFull
        {
            static constexpr const char* name     = "frame.vertex_pool_registry_full";
            static constexpr const char* message  = "顶点池注册表已满(上限 {0}),该顶点源渲染不出来";
            static constexpr ERecovery   recovery = ERecovery::Permanent;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 图重编与格式键在同一帧里互相触发 —— 每帧都在重编图,帧率会被拖垮。
        struct GraphRecompileThrash
        {
            static constexpr const char* name     = "frame.graph_recompile_thrash";
            static constexpr const char* message  = "渲染图在同一帧内被反复重编(格式键抖动)";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        /// 场景的目标布局里没有 SceneColor 槽 —— 图编不出来,这一帧是空的。
        struct NoSceneColorTarget
        {
            static constexpr const char* name     = "frame.no_scene_color_target";
            static constexpr const char* message  = "场景的目标布局没有 SceneColor 槽,本帧无内容可画";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{};
        };

        /// 送进来的绑定没带 RenderTargetLayout。调用方 bug:呈现路径本应逐相位设好。
        struct BindingHasNoLayout
        {
            static constexpr const char* name     = "frame.binding_has_no_layout";
            static constexpr const char* message  = "渲染绑定没有携带 RenderTargetLayout,本次绘制跳过";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        /// 阴影图集的槽位分配在相邻两次**重建**间变了(重建由指纹 MISS 触发,
        /// 中间可能隔很多帧)。某盏灯的槽位得而复失会让它照亮的区域闪烁 ——
        /// 症状是「画面里一部分的阴影消失几毫秒」。偶发一次是正常的场景变化,
        /// 持续累加就是图集接近饱和、边缘灯在换位(看第一个参数:换位槽数)。
        ///
        /// (原消息只打一对总数,「55 → 55」什么都说明不了 —— 洗牌恰恰是总数
        ///  不变、内容重排;现在第一参就是真正变动的槽位数。)
        struct ShadowAtlasAssignmentChurn
        {
            static constexpr const char* name     = "frame.shadow_atlas_assignment_churn";
            static constexpr const char* message  = "阴影图集分配变动:{0} 个槽位换位(总槽位 {1} → {2})";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint, EErrorArg::Uint, EErrorArg::Uint};
        };

        /// 上层给的目标布局缺了某个特性声明要读/要写的槽,引擎就地补上了。
        /// 补的是**上层没要求的东西** —— 能跑,但上层拿到的目标与它以为的不同。
        struct TargetLayoutAmended
        {
            static constexpr const char* name     = "frame.target_layout_amended";
            static constexpr const char* message  = "目标布局被就地修正:槽 {0}(特性声明需要它)";
            static constexpr ERecovery   recovery = ERecovery::NeedsInput;
            static constexpr ErrorArgs   args{EErrorArg::Uint};
        };

        /// 上传完成水位查不动,已完成的上传要等下一 tick 才能回收。
        struct TimelineQueryFailed
        {
            static constexpr const char* name     = "frame.timeline_query_failed";
            static constexpr const char* message  = "vkGetSemaphoreCounterValue 失败({0}),上传完成回收推迟";
            static constexpr ERecovery   recovery = ERecovery::Retryable;
            static constexpr ErrorArgs   args{EErrorArg::VkResult};
        };
    } // namespace frame

    // ── 通用兜底 ─────────────────────────────────────────────────────────
    //
    // 这两个只说明「出错了」,对调用方没有分诊价值,所以**新代码不要用它们**:
    // 为你的失败情形登记一个具体类型,把当时能拿到的数字放进实参槽。
    namespace internal
    {
        struct InvalidArgument
        {
            static constexpr const char* name     = "internal.invalid_argument";
            static constexpr const char* message  = "参数不合法";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };

        struct Unspecified
        {
            static constexpr const char* name     = "internal.unspecified";
            static constexpr const char* message  = "未分类的内部错误";
            static constexpr ERecovery   recovery = ERecovery::Bug;
            static constexpr ErrorArgs   args{};
        };
    } // namespace internal

} // namespace lux::render::err

/// 引擎自带错误类型的完整列表。构造 RenderErrorRegistry 时逐项登记,并在编译期两两
/// 比对 name 查重(见 RenderErrorRegistry.hpp 的 static_assert)。
/// 名字全限定,所以这个宏可以在任意命名空间里展开。
#define LUX_RENDER_ERROR_LIST(X)                                        \
    X(::lux::render::err::device::VulkanObjectCreationFailed)            \
    X(::lux::render::err::device::VulkanCallFailed)                      \
    X(::lux::render::err::device::FrameLifecycleCallFailed)              \
    X(::lux::render::err::device::SwapchainUnavailable)                   \
    X(::lux::render::err::device::SwapchainSurfaceChanged)                \
    X(::lux::render::err::device::SwapchainVulkanCallFailed)              \
    X(::lux::render::err::device::SwapchainConfigurationInvalid)          \
    X(::lux::render::err::device::SwapchainBuildContractViolated)         \
    X(::lux::render::err::device::RenderPassCreationFailed)              \
    X(::lux::render::err::device::InvalidFramesInFlight)                 \
    X(::lux::render::err::shader::BuiltinUnavailable)                    \
    X(::lux::render::err::shader::HandleStale)                           \
    X(::lux::render::err::shader::SourceBytesUnavailable)                \
    X(::lux::render::err::shader::SpirvParseFailed)                      \
    X(::lux::render::err::shader::SetPartiallyRelocatable)               \
    X(::lux::render::err::shader::ReflectionIsSubsetOfModule)            \
    X(::lux::render::err::shader::ReflectionOutOfSyncWithSpirv)          \
    X(::lux::render::err::memory::GpuAllocationFailed)                   \
    X(::lux::render::err::memory::OutOfMemory)                           \
    X(::lux::render::err::memory::CapacityExhausted)                     \
    X(::lux::render::err::asset::Invalid)                                \
    X(::lux::render::err::asset::UnsupportedFormat)                      \
    X(::lux::render::err::resource::NotFound)                            \
    X(::lux::render::err::resource::AlreadyExists)                       \
    X(::lux::render::err::resource::TypeMismatch)                        \
    X(::lux::render::err::resource::UnsupportedType)                     \
    X(::lux::render::err::resource::ModifyFailed)                        \
    X(::lux::render::err::pipeline::StageMergeStateConflict)              \
    X(::lux::render::err::pipeline::DomainSlotMismatch)                   \
    X(::lux::render::err::pipeline::BindingNotInContract)                 \
    X(::lux::render::err::pipeline::BindingPositionMismatch)              \
    X(::lux::render::err::pipeline::BindingTypeMismatch)                  \
    X(::lux::render::err::pipeline::DomainLayoutNotInitialised)           \
    X(::lux::render::err::pipeline::ContractResourceNotMerged)            \
    X(::lux::render::err::pipeline::ReflectedSetLayoutMissing)            \
    X(::lux::render::err::pipeline::VariantBudgetExhausted)               \
    X(::lux::render::err::pipeline::GraphicsCreationFailed)               \
    X(::lux::render::err::descriptor::DomainWriteTargetEmpty)             \
    X(::lux::render::err::graph::CompiledGraphInvalid)                   \
    X(::lux::render::err::graph::DependencyCycle)                        \
    X(::lux::render::err::graph::ImportedResourceIncomplete)             \
    X(::lux::render::err::graph::ReferencedResourceHasNoProducer)        \
    X(::lux::render::err::graph::PassStarvedOfInput)                     \
    X(::lux::render::err::graph::ComputePassMissingPipeline)             \
    X(::lux::render::err::graph::KernelEmitterMissing)                   \
    X(::lux::render::err::graph::ComputePassPipelineStale)               \
    X(::lux::render::err::graph::CrossQueueTransferRequired)             \
    X(::lux::render::err::graph::ConditionalPassOwnsClear)               \
    X(::lux::render::err::graph::ConditionalPassWritesUnconditionalRead) \
    X(::lux::render::err::graph::AllConditionalPassGroup)                \
    X(::lux::render::err::graph::AttachmentFormatUnknown)                \
    X(::lux::render::err::graph::AttachmentPlanInvalid)                  \
    X(::lux::render::err::graph::DomainSlotWithoutDomainSets)            \
    X(::lux::render::err::graph::EngineSetUnresolved)                    \
    X(::lux::render::err::graph::SharedSetBindingTypeConflict)           \
    X(::lux::render::err::graph::VariantLayoutIncompatible)              \
    X(::lux::render::err::graph::VariantSetCountMismatch)                \
    X(::lux::render::err::graph::DomainBindingRangeOverlap)              \
    X(::lux::render::err::graph::MergedLayoutExceedsSetBudget)           \
    X(::lux::render::err::graph::ProjectedLayoutExceedsSetBudget)        \
    X(::lux::render::err::graph::AuthoredDerivedSlotMismatch)            \
    X(::lux::render::err::graph::TransientUsageViolation)                \
    X(::lux::render::err::graph::UsageUnderdeclared)                     \
    X(::lux::render::err::graph::TransientDescriptorLayoutMismatch)      \
    X(::lux::render::err::graph::ResourceBindingConsumeMissing)          \
    X(::lux::render::err::graph::SlotFormatUnmapped)                     \
    X(::lux::render::err::feature::ResourceInitFailed)                   \
    X(::lux::render::err::feature::FactoryHasNoCreateFn)                 \
    X(::lux::render::err::feature::TypeIdCollision)                      \
    X(::lux::render::err::feature::FeatureNameCollision)                 \
    X(::lux::render::err::feature::TypeNotRegistered)                    \
    X(::lux::render::err::feature::FeatureTypeInUse)                     \
    X(::lux::render::err::feature::AlreadyInstalled)                     \
    X(::lux::render::err::feature::ConflictsWithInstalled)               \
    X(::lux::render::err::feature::DependencyMissing)                    \
    X(::lux::render::err::feature::LevelProfileMissing)                  \
    X(::lux::render::err::feature::LevelRequirementsUnmet)               \
    X(::lux::render::err::feature::StillRequiredByAnother)               \
    X(::lux::render::err::feature::RuntimeDisableUnsupported)            \
    X(::lux::render::err::feature::HandleStale)                          \
    X(::lux::render::err::scene::NotFound)                               \
    X(::lux::render::err::scene::SpatialOriginRebaseRejected)           \
    X(::lux::render::err::validation::LayerReport)                       \
    X(::lux::render::err::validation::EventsDropped)                     \
    X(::lux::render::err::comm::RequestInvalid)                          \
    X(::lux::render::err::comm::RequestNotReady)                         \
    X(::lux::render::err::comm::ChannelStopping)                         \
    X(::lux::render::err::comm::PayloadSizeMismatch)                     \
    X(::lux::render::err::comm::ReplyTypeMismatch)                       \
    X(::lux::render::err::comm::ConfigVersionMismatch)                   \
    X(::lux::render::err::comm::DescriptorLayoutVersionMismatch)         \
    X(::lux::render::err::comm::UnknownExtensionFlags)                   \
    X(::lux::render::err::comm::AttachmentIndexOutOfRange)               \
    X(::lux::render::err::comm::AttachmentTypeMismatch)                  \
    X(::lux::render::err::comm::InvalidOpcode)                           \
    X(::lux::render::err::comm::PayloadOutOfBounds)                      \
    X(::lux::render::err::comm::UnknownTypeId)                           \
    X(::lux::render::err::comm::TypeIdGenerationMismatch)                \
    X(::lux::render::err::comm::HandlerNotRegistered)                    \
    X(::lux::render::err::comm::FeatureOperationUnavailable)             \
    X(::lux::render::err::comm::BulkPayloadNotMultiple)                  \
    X(::lux::render::err::comm::BulkPayloadMisaligned)                   \
    X(::lux::render::err::lighting::LocalReadUnsupported)                \
    X(::lux::render::err::lighting::ReadModeInvalid)                     \
    X(::lux::render::err::lighting::ReadModeScopeMismatch)               \
    X(::lux::render::err::lighting::ShadowTechniqueMismatch)             \
    X(::lux::render::err::frame::SkinningInputPoolUnavailable)           \
    X(::lux::render::err::frame::BonePaletteFull)                        \
    X(::lux::render::err::frame::SkinnedVertexPoolExhausted)             \
    X(::lux::render::err::frame::SkinningDispatchParamsOverflow)         \
    X(::lux::render::err::frame::VertexPoolRegistryFull)                 \
    X(::lux::render::err::frame::GraphRecompileThrash)                   \
    X(::lux::render::err::frame::NoSceneColorTarget)                     \
    X(::lux::render::err::frame::BindingHasNoLayout)                     \
    X(::lux::render::err::frame::ShadowAtlasAssignmentChurn)             \
    X(::lux::render::err::frame::TargetLayoutAmended)                    \
    X(::lux::render::err::frame::TimelineQueryFailed)                    \
    X(::lux::render::err::internal::InvalidArgument)                     \
    X(::lux::render::err::internal::Unspecified)
