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
        RGBuilder();                                       // defn in .cpp (Impl complete there)
        ~RGBuilder();
        RGBuilder(RGBuilder&&) noexcept;
        RGBuilder& operator=(RGBuilder&&) noexcept;
        RGBuilder(const RGBuilder&)            = delete;    // pimpl: move-only (verified no in-tree copy)
        RGBuilder& operator=(const RGBuilder&) = delete;

        // Internal-only escape hatch for the in-tree compiler/recorder + tests.
        // Returns the in-progress description by ref; RGGraphDescription is a
        // project-internal type, so this never reaches the install-public surface.
        [[nodiscard]] RGGraphDescription&       graphInternal() noexcept;
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
        /// this is the missing Builder entry point. (M1)
        [[nodiscard]] RGResourceHandle createPersistentTexture(std::string_view name, const RGTextureDescription& desc);

        /// Create a PING_PONG texture with `ring_size` (>=2) physical copies rotated
        /// by frame. Returns a handle pair — write current(), read previous() (last
        /// frame's copy). RG owns the copies + per-frame selection; passes express
        /// the cross-frame read as build.write(current()) + reader.read(previous()).
        /// For HZB / TAA / temporal resources. (M1)
        [[nodiscard]] RGRingResourceHandle createPingPong(std::string_view name,
                                       const RGTextureDescription& desc,
                                       uint32_t ring_size = 2);

        /// PING_PONG buffer variant — N rotating copies of a buffer (e.g. CPU-written
        /// companion data that must stay paired with a ping-pong texture by frame
        /// parity). Same current()/previous() semantics as createPingPong. (M2)
        [[nodiscard]] RGRingResourceHandle createPingPongBuffer(std::string_view name,
                                       const RGBufferDescription& desc,
                                       uint32_t ring_size = 2);

        // Import external texture (e.g., Swapchain Backbuffer)
        // Allow specifying the expected state of the resource at the end of the Graph (final_layout)
        [[nodiscard]] RGResourceHandle importTexture(std::string_view name, 
                                       const RGTextureDescription& desc, 
                                       const RGImportedResourceInfo& import_info);

        /// Import a slotted texture: same as importTexture() but also registers
        /// the resource in the slot table so the recorder can inject per-frame
        /// VkImage/VkImageView handles via RGFrameContext::imported_slots.
        [[nodiscard]] RGResourceHandle importSlottedTexture(TargetSlot slot,
                                       std::string_view name,
                                       const RGTextureDescription& desc,
                                       RGImportedResourceInfo import_info);

        // Import external buffer (e.g., persistent SSBOs, indirect draw buffers)
        [[nodiscard]] RGResourceHandle importBuffer(std::string_view name,
                                       const RGBufferDescription& desc,
                                       const RGImportedBufferInfo& import_info);

        // -----------------------------------------------------------------
        // External (feature-owned) resource tracking (M3 layer 2)
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
        [[nodiscard]] RGResourceHandle referenceTexture(std::string_view name, ERGReference ref = ERGReference::Optional);
        [[nodiscard]] RGResourceHandle referenceBuffer(std::string_view name, ERGReference ref = ERGReference::Optional);

        /// Subscribe (by name) to a ping-pong resource produced by ANOTHER feature.
        /// Returns {current, previous}; the consumer reads previous() (last frame).
        /// ERGReference::Required → a missing producer fails compilation fast;
        /// Optional → the reader is pruned (consumer degrades). Decouples consumer
        /// from producer: no #include, no concrete type, just a name + contract. (M2)
        [[nodiscard]] RGRingResourceHandle referencePingPong(std::string_view name, ERGReference ref = ERGReference::Optional);

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
         *       .write(color_target, lux::common::ETextureRole::COLOR_ATTACHMENT)
         *       .setPipeline(pipeline)
         *       .setRecorder([](const PassRecordContext& ctx) { ... });
         */
        [[nodiscard]] RGPassBuilder addPass(std::string_view name, ERGPassType type);

        // Build final RGGraphDescription
        // Requires calling on rvalue to avoid copy:
        //   RGGraphDescription graph = std::move(builder).build();
        [[nodiscard]] RGGraphDescription build() &&;   // defn in .cpp

        // -----------------------------------------------------------------
        // Transient descriptor set creation
        // -----------------------------------------------------------------
        /// Register a transient descriptor set description in the graph.
        /// Returns an opaque handle used by RGPassBuilder::bindTransientDS().
        [[nodiscard]] RGTransientDSHandle createTransientDS(
            std::string_view              name,
            VkDescriptorSetLayout         layout,
            std::vector<RGDescriptorWrite> writes);

    private:
        struct Impl;                      // defined in RGBuilder.cpp
        std::unique_ptr<Impl> impl_;      // hides RGGraphDescription + name->index map (was by-value members)
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
        RGPassBuilder& read(RGResourceHandle res, lux::common::ETextureRole role = lux::common::ETextureRole::SAMPLED);
        RGPassBuilder& write(RGResourceHandle res, lux::common::ETextureRole role = lux::common::ETextureRole::COLOR_ATTACHMENT);
        RGPassBuilder& readWrite(RGResourceHandle res, lux::common::ETextureRole role = lux::common::ETextureRole::UNORDERED_ACCESS);

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
        RGPassBuilder& setComputePipeline(ComputePipelineHandle pipeline);  ///< For COMPUTE passes.
        /// Explicit ordering: this pass must execute after the named pass.
        /// This controls WAW iteration order in the dependency analyzer.
        RGPassBuilder& after(std::string_view pass_name);

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

        // Conditional execution support
        RGPassBuilder& setCondition(PassConditionFn cond);
        RGPassBuilder& setEnabled(bool enabled);
        /// Force the pass to use its own pass-level pipeline for all render items,
        /// ignoring per-item pipeline handles.  Useful for depth-only prepass passes.
        RGPassBuilder& forcePassPipeline(bool force = true);

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
            lux::common::ETextureRole consume_tex_role = lux::common::ETextureRole::SAMPLED);

    private:
        RGPassBuilder(RGGraphDescription& graph, uint32_t pass_index) noexcept
            : graph_{ &graph }, pass_index_{ pass_index }{}

        // Internal accessor replacing the former public description().
        [[nodiscard]] RGPassDescription& pass() noexcept;

        RGGraphDescription* graph_{ nullptr };
        uint32_t            pass_index_{ std::numeric_limits<uint32_t>::max() };
    };

} // namespace lux::render
