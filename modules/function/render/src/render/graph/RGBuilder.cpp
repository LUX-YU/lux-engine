#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>

#include <unordered_map>
#include <string>

namespace lux::render
{
    // Transparent hash/equal for heterogeneous string_view lookup (P-08).
    // Moved out of the public header (RGBuilder is pimpl now) — builder-internal only.
    namespace {
        struct StringHash {
            using is_transparent = void;
            size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
        };
        struct StringEqual {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
        };
        using LocalResourceMap = std::unordered_map<std::string, uint32_t, StringHash, StringEqual>;
    } // namespace

    // =====================================================================
    // RGBuilder Implementation
    // =====================================================================

    // Hidden state — keeps RGGraphDescription (and its SmallVector members) out of
    // the public RGBuilder.hpp object layout.
    struct RGBuilder::Impl
    {
        RGGraphDescription graph;
        LocalResourceMap   resource_name_to_index;
    };

    RGBuilder::RGBuilder() : impl_(std::make_unique<Impl>()) {}
    RGBuilder::~RGBuilder() = default;
    RGBuilder::RGBuilder(RGBuilder&&) noexcept = default;
    RGBuilder& RGBuilder::operator=(RGBuilder&&) noexcept = default;

    RGGraphDescription&       RGBuilder::graphInternal() noexcept       { return impl_->graph; }
    const RGGraphDescription& RGBuilder::graphInternal() const noexcept { return impl_->graph; }
    RGGraphDescription        RGBuilder::build() &&                     { return std::move(impl_->graph); }

    RGBuilder& RGBuilder::reset() noexcept
    {
        impl_->graph.resources.clear();
        impl_->graph.passes.clear();
        impl_->resource_name_to_index.clear();
        return *this;
    }

    RGResourceHandle RGBuilder::createTexture(std::string_view name, const RGTextureDescription& desc)
    {
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::TEXTURE;
        res.lifetime = ERGResourceLifetime::TRANSIENT;
        res.desc = desc;

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::createBuffer(std::string_view name, const RGBufferDescription& desc)
    {
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::BUFFER;
        res.lifetime = ERGResourceLifetime::TRANSIENT;
        res.desc = desc;

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::createPersistentTexture(std::string_view name, const RGTextureDescription& desc)
    {
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::TEXTURE;
        res.lifetime = ERGResourceLifetime::PERSISTENT;
        res.desc = desc;

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGRingResourceHandle RGBuilder::createPingPong(std::string_view name,
                                                   const RGTextureDescription& desc,
                                                   uint32_t ring_size)
    {
        if (ring_size < 2u) ring_size = 2u;

        const uint32_t cur_idx  = static_cast<uint32_t>(impl_->graph.resources.size());
        const uint32_t prev_idx = cur_idx + 1u;

        // CURRENT: owns the ring_size physical copies (ring_phase 0).
        RGResourceDescription cur{};
        cur.name          = std::string{ name };
        cur.name_hash     = lux::cxx::algorithm::fnv1a(cur.name);
        cur.type          = ERGResourceType::TEXTURE;
        cur.lifetime      = ERGResourceLifetime::PING_PONG;
        cur.desc          = desc;
        cur.ring_size     = ring_size;
        cur.ring_phase    = 0u;
        cur.pingpong_peer = static_cast<int32_t>(prev_idx);
        impl_->graph.resources.push_back(std::move(cur));
        impl_->resource_name_to_index[std::string{ name }] = cur_idx;

        // PREVIOUS: no allocation (ring_phase 1) — resolves to the current
        // resource's copy from one frame earlier. Distinct name so the lookup
        // table doesn't collide with current.
        const std::string prev_name = std::string{ name } + ".prev";
        RGResourceDescription prev{};
        prev.name          = prev_name;
        prev.name_hash     = lux::cxx::algorithm::fnv1a(prev.name);
        prev.type          = ERGResourceType::TEXTURE;
        prev.lifetime      = ERGResourceLifetime::PING_PONG;
        prev.desc          = desc;
        prev.ring_size     = ring_size;
        prev.ring_phase    = 1u;
        prev.pingpong_peer = static_cast<int32_t>(cur_idx);
        impl_->graph.resources.push_back(std::move(prev));
        impl_->resource_name_to_index[prev_name] = prev_idx;

        return RGRingResourceHandle{ RGResourceHandle{ cur_idx }, RGResourceHandle{ prev_idx } };
    }

    RGRingResourceHandle RGBuilder::createPingPongBuffer(std::string_view name,
                                                         const RGBufferDescription& desc,
                                                         uint32_t ring_size)
    {
        if (ring_size < 2u) ring_size = 2u;

        const uint32_t cur_idx  = static_cast<uint32_t>(impl_->graph.resources.size());
        const uint32_t prev_idx = cur_idx + 1u;

        RGResourceDescription cur{};
        cur.name          = std::string{ name };
        cur.name_hash     = lux::cxx::algorithm::fnv1a(cur.name);
        cur.type          = ERGResourceType::BUFFER;
        cur.lifetime      = ERGResourceLifetime::PING_PONG;
        cur.desc          = desc;
        cur.ring_size     = ring_size;
        cur.ring_phase    = 0u;
        cur.pingpong_peer = static_cast<int32_t>(prev_idx);
        impl_->graph.resources.push_back(std::move(cur));
        impl_->resource_name_to_index[std::string{ name }] = cur_idx;

        const std::string prev_name = std::string{ name } + ".prev";
        RGResourceDescription prev{};
        prev.name          = prev_name;
        prev.name_hash     = lux::cxx::algorithm::fnv1a(prev.name);
        prev.type          = ERGResourceType::BUFFER;
        prev.lifetime      = ERGResourceLifetime::PING_PONG;
        prev.desc          = desc;
        prev.ring_size     = ring_size;
        prev.ring_phase    = 1u;
        prev.pingpong_peer = static_cast<int32_t>(cur_idx);
        impl_->graph.resources.push_back(std::move(prev));
        impl_->resource_name_to_index[prev_name] = prev_idx;

        return RGRingResourceHandle{ RGResourceHandle{ cur_idx }, RGResourceHandle{ prev_idx } };
    }

    RGResourceHandle RGBuilder::importTexture(std::string_view name,
                                       const RGTextureDescription& desc,
                                       const RGImportedResourceInfo& import_info)
    {
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::TEXTURE;
        res.lifetime = ERGResourceLifetime::IMPORTED;
        res.desc = desc;
        res.import_info = std::make_unique<RGImportedResourceInfo>(import_info);

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::importBuffer(std::string_view name,
                                       const RGBufferDescription& desc,
                                       const RGImportedBufferInfo& import_info)
    {
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::BUFFER;
        res.lifetime = ERGResourceLifetime::IMPORTED;
        res.desc = desc;
        res.import_buffer_info = std::make_unique<RGImportedBufferInfo>(import_info);

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::importSlottedTexture(TargetSlot slot,
                                       std::string_view name,
                                       const RGTextureDescription& desc,
                                       RGImportedResourceInfo import_info)
    {
        import_info.slot = slot;
        return importTexture(name, desc, import_info);
    }

    RGResourceHandle RGBuilder::trackExternalBuffer(std::string_view name)
    {
        // De-dup: many passes may consume the same feature-owned resource.
        if (auto it = impl_->resource_name_to_index.find(name); it != impl_->resource_name_to_index.end())
            return RGResourceHandle{ it->second };

        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::BUFFER;
        res.lifetime = ERGResourceLifetime::EXTERNAL;
        res.desc = RGBufferDescription{};   // never allocated — keep variant type consistent with BUFFER

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::trackExternalTexture(std::string_view name)
    {
        if (auto it = impl_->resource_name_to_index.find(name); it != impl_->resource_name_to_index.end())
            return RGResourceHandle{ it->second };

        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::TEXTURE;
        res.lifetime = ERGResourceLifetime::EXTERNAL;
        // desc defaults to RGTextureDescription (variant's first alternative) — unused.

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::referenceTexture(std::string_view name, ERGReference ref)
    {
        // If the resource already exists, return its handle directly
        if (auto it = impl_->resource_name_to_index.find(name); it != impl_->resource_name_to_index.end())
            return RGResourceHandle{ it->second };

        // Create a forward-reference placeholder
        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::TEXTURE;
        res.lifetime = ERGResourceLifetime::FORWARD_REFERENCE;
        res.reference_mode = ref;
        // desc left default — will be resolved from the actual resource at compile time

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGResourceHandle RGBuilder::referenceBuffer(std::string_view name, ERGReference ref)
    {
        if (auto it = impl_->resource_name_to_index.find(name); it != impl_->resource_name_to_index.end())
            return RGResourceHandle{ it->second };

        const uint32_t index = static_cast<uint32_t>(impl_->graph.resources.size());

        RGResourceDescription res{};
        res.name = std::string{ name };
        res.name_hash = lux::cxx::algorithm::fnv1a(res.name);
        res.type = ERGResourceType::BUFFER;
        res.lifetime = ERGResourceLifetime::FORWARD_REFERENCE;
        res.reference_mode = ref;

        impl_->graph.resources.push_back(std::move(res));
        impl_->resource_name_to_index[std::string{ name }] = index;

        return RGResourceHandle{ index };
    }

    RGRingResourceHandle RGBuilder::referencePingPong(std::string_view name, ERGReference ref)
    {
        // Mirror createPingPong's naming: "<name>" = current, "<name>.prev" = previous.
        // Both resolve at compile time to the producer's ping-pong pair (or stay
        // forward-refs that the required/optional contract then handles).
        RGResourceHandle cur  = referenceTexture(name, ref);
        RGResourceHandle prev = referenceTexture(std::string{ name } + ".prev", ref);
        return RGRingResourceHandle{ cur, prev };
    }

    RGResourceHandle RGBuilder::findResource(std::string_view name) const noexcept
    {
        auto it = impl_->resource_name_to_index.find(name);
        if (it != impl_->resource_name_to_index.end())
        {
            return RGResourceHandle{ it->second };
        }
        return invalid_rg_resource_handle;
    }

    const RGResourceDescription* RGBuilder::getResourceDescription(RGResourceHandle handle) const noexcept
    {
        if (handle.isInvalid() || handle.index >= impl_->graph.resources.size())
        {
            return nullptr;
        }
        return &impl_->graph.resources[handle.index];
    }

    RGPassBuilder RGBuilder::addPass(std::string_view name, ERGPassType type)
    {
        RGPassDescription pass{};
        pass.name = std::string{ name };
        pass.name_hash = lux::cxx::algorithm::fnv1a(pass.name);
        pass.type = type;

        const uint32_t index = static_cast<uint32_t>(impl_->graph.passes.size());
        impl_->graph.passes.push_back(std::move(pass));

        return RGPassBuilder{ impl_->graph, index };
    }

    // =====================================================================
    // RGPassBuilder Implementation
    // =====================================================================
    RGPassDescription& RGPassBuilder::pass() noexcept
    {
        return graph_->passes[pass_index_];
    }

    RGPassBuilder& RGPassBuilder::setPipelineVariantFeatures(std::span<const uint32_t> feature_masks)
    {
        auto& vf = pass().pipeline_variant_features;   // lux::cxx::SmallVector<uint32_t, 4>
        vf.clear();
        vf.reserve(feature_masks.size());
        for (uint32_t m : feature_masks) vf.push_back(m);
        return *this;
    }

    // Helper: resolve array_layers for a texture resource (returns 1 for non-array / non-texture).
    static uint32_t resolveLayerCount(const RGGraphDescription& graph, RGResourceHandle handle)
    {
        if (handle.index < graph.resources.size()) {
            const auto* tex = std::get_if<RGTextureDescription>(&graph.resources[handle.index].desc);
            if (tex && tex->dimension == lux::common::ETextureDimension::TEX_2D_ARRAY)
                return tex->array_layers;
        }
        return 1;
    }

    RGPassBuilder& RGPassBuilder::inputRead(RGResourceHandle handle, uint32_t attachment_idx)
    {
        RGPassTextureRef ref{};
        ref.resource = handle;
        ref.role = lux::common::ETextureRole::INPUT_ATTACHMENT;
        ref.usage = ERGResourceUsage::READ;
        ref.input_attachment_index = attachment_idx;
        ref.range.base_mip_level = 0;
        ref.range.level_count = 1;
        ref.range.base_array_layer = 0;
        ref.range.layer_count = resolveLayerCount(*graph_, handle);

        pass().textures.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::read(RGResourceHandle handle, lux::common::ETextureRole role)
    {
        RGPassTextureRef ref{};
        ref.resource = handle;
        ref.role = role;
        ref.usage = ERGResourceUsage::READ;
        // Default range — auto-expand for TEX_2D_ARRAY
        ref.range.base_mip_level = 0;
        ref.range.level_count = 1;
        ref.range.base_array_layer = 0;
        ref.range.layer_count = resolveLayerCount(*graph_, handle);

        pass().textures.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::write(RGResourceHandle handle, lux::common::ETextureRole role)
    {
        RGPassTextureRef ref{};
        ref.resource = handle;
        ref.role = role;
        ref.usage = ERGResourceUsage::WRITE;
        // Default range — auto-expand for TEX_2D_ARRAY
        ref.range.base_mip_level = 0;
        ref.range.level_count = 1;
        ref.range.base_array_layer = 0;
        ref.range.layer_count = resolveLayerCount(*graph_, handle);

        pass().textures.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::readWrite(RGResourceHandle handle, lux::common::ETextureRole role)
    {
        RGPassTextureRef ref{};
        ref.resource = handle;
        ref.role = role;
        ref.usage = ERGResourceUsage::READ_WRITE;
        // Default range — auto-expand for TEX_2D_ARRAY
        ref.range.base_mip_level = 0;
        ref.range.level_count = 1;
        ref.range.base_array_layer = 0;
        ref.range.layer_count = resolveLayerCount(*graph_, handle);

        pass().textures.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::read(RGResourceHandle handle, ERGBufferRole role)
    {
        RGPassBufferRef ref{};
        ref.resource = handle;
        ref.usage = ERGResourceUsage::READ;
        ref.role = role;
        ref.offset = 0;
        ref.size = 0;

        pass().buffers.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::write(RGResourceHandle handle, ERGBufferRole role)
    {
        RGPassBufferRef ref{};
        ref.resource = handle;
        ref.usage = ERGResourceUsage::WRITE;
        ref.role = role;
        ref.offset = 0;
        ref.size = 0;

        pass().buffers.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::readWrite(RGResourceHandle handle, ERGBufferRole role)
    {
        RGPassBufferRef ref{};
        ref.resource = handle;
        ref.usage = ERGResourceUsage::READ_WRITE;
        ref.role = role;
        ref.offset = 0;
        ref.size = 0;

        pass().buffers.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setPipeline(GraphicsPipelineHandle pipeline)
    {
        pass().pipeline_template = pipeline;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setComputePipeline(ComputePipelineHandle pipeline)
    {
        pass().compute_pipeline_handle = pipeline;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::addPipeline(GraphicsPipelineHandle pipeline)
    {
        pass().additional_pipelines.push_back(pipeline);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::after(std::string_view pass_name)
    {
        pass().after_passes.emplace_back(pass_name);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::stage(ERenderStage s)
    {
        pass().stage = s;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::withPhase(render_phase_id phase)
    {
        pass().phase_mask |= phaseBit(phase);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::withPhases(std::initializer_list<render_phase_id> phases)
    {
        for (auto p : phases) pass().phase_mask |= phaseBit(p);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setPhaseMask(uint64_t mask)
    {
        pass().phase_mask = mask;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setRecorder(PassRecordFn fn)
    {
        pass().recorder = std::move(fn);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setKernelFn(PassRecordFn fn)
    {
        pass().kernel_fn = std::move(fn);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setKernel(std::string_view name)
    {
        pass().kernel_id = KernelRegistry::instance().idOf(name);
        pass().kernel_config = {};
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setKernel(std::string_view name, const KernelConfigBlob& config)
    {
        pass().kernel_id = KernelRegistry::instance().idOf(name);
        pass().kernel_config = config;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setKernel(KernelTypeId id)
    {
        pass().kernel_id = id;
        pass().kernel_config = {};
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setKernel(KernelTypeId id, const KernelConfigBlob& config)
    {
        pass().kernel_id = id;
        pass().kernel_config = config;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setCondition(PassConditionFn cond)
    {
        pass().condition = std::move(cond);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setEnabled(bool enabled)
    {
        pass().enabled = enabled;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::forcePassPipeline(bool force)
    {
        pass().force_pass_pipeline = force;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setManualViewport(bool manual)
    {
        pass().manual_viewport = manual;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::markSideEffect(bool side_effect)
    {
        pass().has_side_effect = side_effect;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::bindImmutableDS(uint32_t slot, VkDescriptorSet ds)
    {
        PassDSBinding b;
        b.slot          = slot;
        b.source        = EDSBindingSource::Immutable;
        b.mode          = EDSBindMode::IMMUTABLE;
        b.immutable_set = ds;
        pass().ds_bindings.push_back(b);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::bindSceneDS(uint32_t slot)
    {
        PassDSBinding b;
        b.slot   = slot;
        b.source = EDSBindingSource::Scene;
        b.mode   = EDSBindMode::VERSIONED;
        pass().ds_bindings.push_back(b);
        return *this;
    }

    RGTransientDSHandle RGBuilder::createTransientDS(
        std::string_view              name,
        VkDescriptorSetLayout         layout,
        std::vector<RGDescriptorWrite> writes)
    {
        RGTransientDSDescription desc;
        desc.name   = std::string(name);
        desc.layout = layout;
        desc.writes = std::move(writes);

        const auto idx = static_cast<uint32_t>(impl_->graph.transient_descriptor_sets.size());
        impl_->graph.transient_descriptor_sets.push_back(std::move(desc));
        return RGTransientDSHandle{ idx };
    }

    RGPassBuilder& RGPassBuilder::bindTransientDS(uint32_t slot, RGTransientDSHandle handle)
    {
        PassDSBinding b;
        b.slot               = slot;
        b.source             = EDSBindingSource::Transient;
        b.mode               = EDSBindMode::VERSIONED;
        b.transient_ds_index = handle.index;
        pass().ds_bindings.push_back(b);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::bindResourceDS(
        uint32_t slot,
        const void* resource,
        DSResolverFn resolver,
        EDSBindMode mode,
        RGResourceHandle declared_consume,
        ERGResourceType consume_type,
        lux::common::ETextureRole consume_tex_role)
    {
        PassDSBinding b;
        b.slot = slot;
        b.source = EDSBindingSource::Resource;
        b.mode = mode;
        b.provider.resource = resource;
        b.provider.resolver = resolver;
        b.declared_consume = declared_consume;
        b.consume_type = consume_type;
        b.consume_tex_role = consume_tex_role;
        pass().ds_bindings.push_back(b);
        return *this;
    }

} // namespace lux::render
