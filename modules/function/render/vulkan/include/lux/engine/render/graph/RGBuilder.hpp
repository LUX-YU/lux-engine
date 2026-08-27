#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <limits>
#include <memory>
#include <span>

#include <lux/engine/render/graph/RGPassTypes.hpp>

namespace lux::render
{
    // Forward declaration
    class RGPassBuilder;

    /**
     * RGBuilder
     *
     * Used for declarative construction of RGGraphDescription.
     * Only responsible for "building the graph", not involved in compilation and execution.
     */

    class LUX_FUNCTION_PUBLIC RGBuilder
    {
    public:
        RGBuilder(); // defn in .cpp (Impl complete there)
        ~RGBuilder();
        RGBuilder(RGBuilder&&) noexcept;
        RGBuilder& operator=(RGBuilder&&) noexcept;
        RGBuilder(const RGBuilder&) = delete; // pimpl: move-only (verified no in-tree copy)
        RGBuilder& operator=(const RGBuilder&) = delete;

        // Internal-only escape hatch for the in-tree compiler/recorder + tests.
        // Returns the in-progress description by ref; RGGraphDescription is a
        // project-internal type, so this never reaches the install-public surface.
        [[nodiscard]] RGGraphDescription& graphInternal() noexcept;
        [[nodiscard]] const RGGraphDescription& graphInternal() const noexcept;

        // Clear builder
        RGBuilder& reset() noexcept;

        // -----------------------------------------------------------------
        // Resource creation methods (can be called before addPass)
        // -----------------------------------------------------------------
        // Create transient texture (automatically allocated and managed by RG)
        [[nodiscard]] RGResourceHandle createTexture(std::string_view name, const RGTextureDescription& desc);

        // Create transient buffer
        [[nodiscard]] RGResourceHandle createBuffer(std::string_view name, const RGBufferDescription& desc);

        /// Create a PERSISTENT texture: a single physical copy that survives across
        /// frames (not pooled / not aliased). The allocator already supports this;
        /// this is the missing Builder entry point.
        [[nodiscard]] RGResourceHandle createPersistentTexture(std::string_view name, const RGTextureDescription& desc);

        /// Create a PING_PONG texture with `ring_size` (>=2) physical copies rotated
        /// by frame. Returns a handle pair — write current(), read previous() (last
        /// frame's copy). RG owns the copies + per-frame selection; passes express
        /// the cross-frame read as build.write(current()) + reader.read(previous()).
        /// For HZB / TAA / temporal resources.
        [[nodiscard]] RGRingResourceHandle
        createPingPong(std::string_view name, const RGTextureDescription& desc, uint32_t ring_size = 2);

        /// PING_PONG buffer variant — N rotating copies of a buffer (e.g. CPU-written
        /// companion data that must stay paired with a ping-pong texture by frame
        /// parity). Same current()/previous() semantics as createPingPong.
        [[nodiscard]] RGRingResourceHandle
        createPingPongBuffer(std::string_view name, const RGBufferDescription& desc, uint32_t ring_size = 2);

        // Import external texture (e.g., Swapchain Backbuffer)
        // Allow specifying the expected state of the resource at the end of the Graph (final_layout)
        [[nodiscard]] RGResourceHandle importTexture(
            std::string_view name,
            const RGTextureDescription& desc,
            const RGImportedResourceInfo& import_info
        );

        /// Import a slotted texture: same as importTexture() but also registers
        /// the resource in the slot table so the recorder can inject per-frame
        /// VkImage/VkImageView handles via RGFrameContext::imported_slots.
        [[nodiscard]] RGResourceHandle importSlottedTexture(
            TargetSlot slot,
            std::string_view name,
            const RGTextureDescription& desc,
            RGImportedResourceInfo import_info
        );

        // Import external buffer (e.g., persistent SSBOs, indirect draw buffers)
        /// Repeated calls with the same name return the existing handle (the later
        /// desc/getter is discarded) — same contract as trackExternalBuffer. This
        /// lets several independently-enabled features share ONE globally-owned
        /// buffer without agreeing on who imports it: first caller wins, rest reuse.
        [[nodiscard]] RGResourceHandle
        importBuffer(std::string_view name, const RGBufferDescription& desc, const RGImportedBufferInfo& import_info);

        // -----------------------------------------------------------------
        // External (feature-owned) resource tracking
        // -----------------------------------------------------------------
        /// Track a feature-owned resource that the RENDER GRAPH must KNOW ABOUT
        /// (for dependency / dead-pass / ordering) but NEVER allocates, fetches a
        /// handle for, or barriers. The feature owns the physical storage and
        /// updates it outside the graph (scene-level Light/Material SSBOs, etc.).
        /// A pass declares it reads this resource via bindResourceDS(..., handle).
        /// Repeated calls with the same name return the existing handle.
        [[nodiscard]] RGResourceHandle trackExternalBuffer(std::string_view name);
        [[nodiscard]] RGResourceHandle trackExternalTexture(std::string_view name);

        // -----------------------------------------------------------------
        // Forward resource references (resolved at compile time)
        // -----------------------------------------------------------------
        /// Create a forward reference to a texture that may be created later by another feature.
        /// Returns a valid handle immediately. At compile time, the compiler resolves it
        /// to the actual TRANSIENT/IMPORTED resource with the same name.
        /// If no matching resource is found, passes using this handle are pruned by dead-pass elimination.
        [[nodiscard]] RGResourceHandle
        referenceTexture(std::string_view name, ERGReference ref = ERGReference::Optional);
        [[nodiscard]] RGResourceHandle
        referenceBuffer(std::string_view name, ERGReference ref = ERGReference::Optional);

        /// Lookup-only probe: the handle if a texture with this name is already
        /// declared, invalid otherwise — NO forward placeholder is created.
        /// ViewContext 的「没有是数据」语义靠它:查询缺席的槽不能在图里
        /// 留下悬空占位。
        [[nodiscard]] RGResourceHandle findTexture(std::string_view name) const noexcept;

        /// Subscribe (by name) to a ping-pong resource produced by ANOTHER feature.
        /// Returns {current, previous}; the consumer reads previous() (last frame).
        /// ERGReference::Required → a missing producer fails compilation fast;
        /// Optional → the reader is pruned (consumer degrades). Decouples consumer
        /// from producer: no #include, no concrete type, just a name + contract.
        [[nodiscard]] RGRingResourceHandle
        referencePingPong(std::string_view name, ERGReference ref = ERGReference::Optional);

        // -----------------------------------------------------------------
        // Resource query methods
        // -----------------------------------------------------------------
        [[nodiscard]] RGResourceHandle findResource(std::string_view name) const noexcept;
        [[nodiscard]] const RGResourceDescription* getResourceDescription(RGResourceHandle handle) const noexcept;

        /**
         * Add a pass and return builder for direct configuration.
         * Allows chained method calls without lambda wrapper.
         *
         * @param name Pass name
         * @param type Pass type
         * @return RGPassBuilder for configuring the pass
         *
         * Example:
         *   builder.addPass("MyPass", ERGPassType::GRAPHICS)
         *       .write(color_target, lux::render::ETextureRole::COLOR_ATTACHMENT)
         *       .setPipeline(pipeline)
         *       .setRecorder([](const PassRecordContext& ctx) { ... });
         */
        [[nodiscard]] RGPassBuilder addPass(std::string_view name, ERGPassType type);

        // -----------------------------------------------------------------
        // 条件链作用域
        // -----------------------------------------------------------------
        /**
         * @brief 开一条**原子跳过链**:作用域存续期间 addPass 出来的每个 pass
         *        自动挂上同一个条件与同一个链标签。
         *
         * 为什么是作用域而不是逐 pass 调用:链的语义是「这几个 pass 要么全跑、要么
         * 全不跑」——链内的 transient 每帧 UNDEFINED 起手,布局链才自洽。此前每个
         * 成员各写一遍 `.setCondition(cond, tag)`,协议是隐式的:**链的边界要数遍
         * 六处调用才知道**,而漏挂一个 pass 只会在编译期被 classifyElectivePasses
         * 捕获(有兜底,但意图分散,且下一个链条特性要重抄一遍样板)。
         *
         * 作用域即边界:漏挂在结构上不可能,链的范围一眼可见。
         *
         * @code
         *   {
         *       auto chain = builder.conditionChain([this] { return hasSelection(); });
         *       builder.addPass("HighlightCull",  ERGPassType::COMPUTE) ...;
         *       builder.addPass("HighlightMask",  ERGPassType::GRAPHICS) ...;
         *   }   // 链在这里闭合
         * @endcode
         *
         * 嵌套是错误(一个 pass 只能属于一条链),`conditionChain` 在已有作用域内
         * 再次调用会触发 renderFatal。
         */
        class LUX_FUNCTION_PUBLIC ConditionChainScope
        {
        public:
            ~ConditionChainScope();
            ConditionChainScope(ConditionChainScope&&) noexcept;
            ConditionChainScope& operator=(ConditionChainScope&&) noexcept;
            ConditionChainScope(const ConditionChainScope&) = delete;
            ConditionChainScope& operator=(const ConditionChainScope&) = delete;

            /// 本链的标签。绝大多数调用方用不到 —— 它存在是为了让少数需要把链
            /// 传进辅助函数的地方(如 addCullAndCompactPasses)仍能显式对齐。
            [[nodiscard]] std::uint64_t tag() const noexcept
            {
                return tag_;
            }

        private:
            friend class RGBuilder;
            ConditionChainScope(RGBuilder& owner, std::uint64_t tag) noexcept : owner_(&owner), tag_(tag)
            {
            }

            RGBuilder* owner_{nullptr};
            std::uint64_t tag_{0};
        };

        [[nodiscard]] ConditionChainScope conditionChain(PassConditionFn condition);

        // Build final RGGraphDescription
        // Requires calling on rvalue to avoid copy:
        //   RGGraphDescription graph = std::move(builder).build();
        [[nodiscard]] RGGraphDescription build() &&; // defn in .cpp

        // -----------------------------------------------------------------
        // Transient descriptor set creation
        // -----------------------------------------------------------------
        /// Register a transient descriptor set description in the graph.
        /// Returns an opaque handle used by RGPassBuilder::bindTransientDS().
        [[nodiscard]] RGTransientDSHandle
        createTransientDS(std::string_view name, VkDescriptorSetLayout layout, std::vector<RGDescriptorWrite> writes);

    private:
        struct Impl;                 // defined in RGBuilder.cpp
        std::unique_ptr<Impl> impl_; // hides RGGraphDescription + name->index map (was by-value members)
    };

    // =====================================================================
    // RGPassBuilder: Used to configure a single pass
    // =====================================================================
    class LUX_FUNCTION_PUBLIC RGPassBuilder
    {
        friend class RGBuilder;

    public:
        RGPassBuilder(const RGPassBuilder&) = default;
        RGPassBuilder& operator=(const RGPassBuilder&) = default;

        RGPassBuilder(RGPassBuilder&&) = default;
        RGPassBuilder& operator=(RGPassBuilder&&) = default;

        // Set the per-variant shader feature masks (index matches pipeline variant:
        // 0 = setPipeline(), 1..N = addPipeline()). Clears any prior contents.
        // Replaces the former public description() escape hatch; std::span keeps the
        // internal SmallVector type off the public signature.
        RGPassBuilder& setPipelineVariantFeatures(std::span<const uint32_t> feature_masks);

        // -----------------------------------------------------------------
        // Texture access declaration (only declares dependency, does not create resource)
        // -----------------------------------------------------------------
        RGPassBuilder& read(RGResourceHandle res, lux::render::ETextureRole role = lux::render::ETextureRole::SAMPLED);
        RGPassBuilder&
        write(RGResourceHandle res, lux::render::ETextureRole role = lux::render::ETextureRole::COLOR_ATTACHMENT);
        RGPassBuilder&
        readWrite(RGResourceHandle res, lux::render::ETextureRole role = lux::render::ETextureRole::UNORDERED_ACCESS);

        /// Declare an input-attachment read (subpass input for Path B / tile-based GPUs).
        /// @param handle          Texture written by a prior subpass in the same render pass.
        /// @param attachment_idx  Input attachment index inside the subpass (0-based).
        RGPassBuilder& inputRead(RGResourceHandle handle, uint32_t attachment_idx);

        // -----------------------------------------------------------------
        // Buffer access declaration (only declares dependency, does not create resource)
        // -----------------------------------------------------------------
        RGPassBuilder& read(RGResourceHandle res, ERGBufferRole role);
        RGPassBuilder& write(RGResourceHandle res, ERGBufferRole role);
        RGPassBuilder& readWrite(RGResourceHandle res, ERGBufferRole role);

        // -----------------------------------------------------------------
        // Pass configuration
        // -----------------------------------------------------------------
        RGPassBuilder& setPipeline(GraphicsPipelineHandle pipeline);
        RGPassBuilder& addPipeline(GraphicsPipelineHandle pipeline);
        RGPassBuilder& setComputePipeline(ComputePipelineHandle pipeline); ///< For COMPUTE passes.
        /// Explicit ordering: this pass must execute after the named pass.
        /// This controls WAW iteration order in the dependency analyzer.
        RGPassBuilder& after(std::string_view pass_name);
        /// Reverse of after(): the NAMED pass must execute after this one.
        RGPassBuilder& before(std::string_view pass_name);

        /// Painter-order sort key (see ERenderStage). Orders this pass against other
        /// passes that write the SAME resource with no data dependency between them,
        /// instead of relying on feature registration order. Default = Opaque.
        RGPassBuilder& stage(ERenderStage s);

        RGPassBuilder& setRecorder(PassRecordFn fn);

        /// Set a lightweight kernel recording callback for the fast-path executor.
        /// Called after common state setup (pipeline, DS, viewport, shared PCs).
        /// The callback handles feature-specific work (custom push constants,
        /// VBO binding, runtime draw/dispatch parameters).
        RGPassBuilder& setKernelFn(PassRecordFn fn);

        /// Set a compile-time kernel for this pass by name (resolved via KernelRegistry).
        /// @param name     Kernel name as registered with LUX_REGISTER_KERNEL.
        /// @param config   Pre-built kernel configuration blob.
        RGPassBuilder& setKernel(std::string_view name, const KernelConfigBlob& config);

        /// Set a compile-time kernel by name with no configuration data.
        RGPassBuilder& setKernel(std::string_view name);

        /// Set a compile-time kernel by pre-resolved numeric ID (advanced / performance path).
        RGPassBuilder& setKernel(KernelTypeId id, const KernelConfigBlob& config);

        /// Set a compile-time kernel by pre-resolved numeric ID with no configuration data.
        RGPassBuilder& setKernel(KernelTypeId id);

        // Phase-based pass filtering
        RGPassBuilder& withPhase(render_phase_id phase);
        RGPassBuilder& withPhases(std::initializer_list<render_phase_id> phases);
        RGPassBuilder& setPhaseMask(uint64_t mask);

        // Conditional execution support. chain_tag != 0 marks this pass as a
        // member of an atomic skip-chain (see RGPassDescription::condition_tag).
        RGPassBuilder& setCondition(PassConditionFn cond, uint64_t chain_tag = 0);
        RGPassBuilder& setEnabled(bool enabled);
        // (forcePassPipeline 已作废删除:它要压制的"逐项管线句柄"来自 DrawPacket
        //  摄取体系,该体系已整套删除;当前 kernel 执行模型下不发逐项 BindPipeline,
        //  pass 级管线本就保持绑定 —— 意图由结构保证,标志位从来没人读。)

        /// When true, the framework skips auto-setting viewport/scissor before
        /// the recorder lambda.  The feature must set them manually.
        RGPassBuilder& setManualViewport(bool manual = true);

        /// Mark this pass as having a side effect outside the RenderGraph — it
        /// writes a FEATURE-OWNED resource the graph cannot see consumed — so
        /// dead-pass elimination never prunes it. See RGPassDescription::has_side_effect.
        RGPassBuilder& markSideEffect(bool side_effect = true);

        // ── Descriptor set binding API ─────────────────────────────────────

        /// Bind a pre-resolved (immutable) descriptor set at the given Vulkan slot.
        /// The handle must remain valid for the lifetime of the graph.
        RGPassBuilder& bindImmutableDS(uint32_t slot, VkDescriptorSet ds);

        /// Bind the per-view scene descriptor set at the given slot.
        /// At record time the framework fills the slot from RGFrameContext::scene_ds.
        RGPassBuilder& bindSceneDS(uint32_t slot);

        /// Bind a transient descriptor set at the given slot.
        /// The DS is allocated per-view per-frame from the graph description.
        RGPassBuilder& bindTransientDS(uint32_t slot, RGTransientDSHandle handle);

        /// Bind a late-bound descriptor set resolved per-frame via a resolver function.
        /// At record time the resolver is called with (resource, frame_slot) to obtain
        /// the VkDescriptorSet for the current FIF slot.
        RGPassBuilder& bindResourceDS(
            uint32_t slot,
            const void* resource,
            DSResolverFn resolver,
            EDSBindMode mode,
            RGResourceHandle declared_consume = {},
            ERGResourceType consume_type = ERGResourceType::BUFFER,
            lux::render::ETextureRole consume_tex_role = lux::render::ETextureRole::SAMPLED
        );

        // ── Bind an engine set by LOGICAL IDENTITY (recommended; the raw-slot-number
        //    overloads above are being retired) ──
        //
        //  The only difference: you say "bind the lighting set" instead of "bind
        //  slot 3". The actual slot number is resolved at graph-compile time from
        //  the slot table of whichever pipeline this pass uses.
        //
        //  Why this matters: the same engine set can legitimately sit at a different
        //  slot in different pipelines (skinning puts the vertex pool at set 1, the
        //  shadow compact layout puts it at set 3) — and once the position is decided
        //  at render-graph compile time, hand-written slot numbers silently become
        //  ALL WRONG WITH NO ERROR. Binding the wrong set just shows up as a broken
        //  image or a VUID-00358 validation error.
        //
        //  Only applies to engine sets. Private/transient sets already belong to
        //  their own pipeline, so they keep using raw slot numbers.

        /// 声明"本 pass 用这个引擎集"——**不传句柄、不传 resolver**。
        ///
        /// 这是旧的 per-set 机制退休后引擎集的唯一绑定形式。此前调用方要交出一个
        /// per-set 实例(或解析它的函数),但域合并之后那个句柄在 record 期被
        /// 直接丢弃、换成场景的域集实例 —— 也就是说调用方一直在传一个**没人
        /// 读的参数**,还得为此持有资源指针、知道它有个 resolveDS。现在没有了:
        /// 说出逻辑身份即可,slot 号在图编译期解析,实例在 record 期解析。
        ///
        /// 适用于 FEATURE / GLOBAL 域的引擎集(Instance / Light / Material /
        /// VertexPool)。BINDLESS 域(Texture)不走这条 —— 那个域的"实例"就是
        /// 全局纹理表本身,句柄是真实输入,继续用 bindImmutableDS。
        ///
        /// declared_consume 仍然要给:它是这条绑定对渲染图声明的**数据依赖**
        /// (集里装的缓冲/纹理由谁产出),图靠它排序与剪枝;不给会掉进
        /// computeDescriptorBindingPlan 的告警。
        RGPassBuilder& useEngineSet(
            EDescriptorSetSlot logical,
            RGResourceHandle declared_consume = {},
            ERGResourceType consume_type = ERGResourceType::BUFFER,
            lux::render::ETextureRole consume_tex_role = lux::render::ETextureRole::SAMPLED
        );

        /// 引擎集的**句柄式**绑定。机制退休后只剩一种合法用法:BINDLESS 域的
        /// 全局纹理表(它没有独立的域实例,全局表自己就是)。FEATURE/GLOBAL 域
        /// 请用 useEngineSet —— 那里传句柄等于传一个会被丢弃的参数。
        RGPassBuilder& bindImmutableDS(EDescriptorSetSlot logical, VkDescriptorSet ds);
        RGPassBuilder& bindSceneDS(EDescriptorSetSlot logical = EDescriptorSetSlot::Scene);

    private:
        RGPassBuilder(RGGraphDescription& graph, uint32_t pass_index) noexcept : graph_{&graph}, pass_index_{pass_index}
        {
        }

        // Internal accessor replacing the former public description().
        [[nodiscard]] RGPassDescription& pass() noexcept;

        RGGraphDescription* graph_{nullptr};
        uint32_t pass_index_{std::numeric_limits<uint32_t>::max()};
    };

} // namespace lux::render
