#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>

#include <lux/cxx/container/HeterogeneousLookup.hpp>

#include <string>

namespace lux::render
{
    // 按名查资源用异构 map:键存 string,查表直接吃 string_view,不构造临时。
    // 这里原先手写了一对 StringHash/StringEqual —— 与 lux::cxx 里的
    // transparent_string_hash 是同一件东西,换成公共的那份。
    namespace {
        using LocalResourceMap = lux::cxx::heterogeneous_map<uint32_t>;
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

        /// 当前打开的条件链(见 RGBuilder::conditionChain)。tag == 0 表示没有。
        /// 作用域存续期间,addPass 出来的每个 pass 都自动继承这两个字段 ——
        /// 链的边界因此是**作用域**,而不是散落在若干处的 setCondition 调用。
        PassConditionFn active_condition{};
        std::uint64_t   active_chain_tag{0};
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
        // 同名复用 —— 与 trackExternalBuffer 的既有契约一致。
        //
        // 存在的意义:**全局共享**的外部缓冲(如所有 GPU 驱动网格特性共用的那条
        // 索引缓冲)没有唯一生产者 —— 前向/延迟/高亮/阴影四个特性各自独立启用,
        // 谁都不能假定别人一定在场。让首个调用者建资源、其余复用同一句柄,四者
        // 便无需任何生产者/消费者约定即可自组织。
        //
        // 对存量调用是**零影响**:现有 importBuffer 调用一律带按特性唯一的前缀名
        // (prefix+"CullMeta"、"MeshShadowSectionTable"、"PC"+label+"Nodes" …),
        // 本就不会撞名。而撞名在今天是**静默有害**的 —— 会造出两条同源资源,且
        // resource_name_to_index 只留后者,referenceBuffer 按名解析到的和调用者
        // 手上的句柄并非同一条。
        if (auto it = impl_->resource_name_to_index.find(name);
            it != impl_->resource_name_to_index.end())
        {
            const auto& existing = impl_->graph.resources[it->second];
            if (existing.type == ERGResourceType::BUFFER
                && existing.lifetime == ERGResourceLifetime::IMPORTED)
            {
                return RGResourceHandle{ it->second };
            }
        }

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

    RGResourceHandle RGBuilder::findTexture(std::string_view name) const noexcept
    {
        if (auto it = impl_->resource_name_to_index.find(name); it != impl_->resource_name_to_index.end())
            return RGResourceHandle{ it->second };
        return RGResourceHandle{};   // invalid ——「没有」是数据
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

        // 条件链作用域内建的 pass 自动入链。显式 setCondition 仍可覆盖 ——
        // 它在 RGPassBuilder 上,发生在这之后。
        if (impl_->active_chain_tag != 0)
        {
            pass.condition     = impl_->active_condition;
            pass.condition_tag = impl_->active_chain_tag;
        }

        const uint32_t index = static_cast<uint32_t>(impl_->graph.passes.size());
        impl_->graph.passes.push_back(std::move(pass));

        return RGPassBuilder{ impl_->graph, index };
    }

    RGBuilder::ConditionChainScope RGBuilder::conditionChain(PassConditionFn condition)
    {
        if (impl_->active_chain_tag != 0)
        {
            // 一个 pass 只能属于一条链 —— 嵌套没有自洽的语义(内层该跟谁原子跳过?)。
            renderFatal("RGBuilder::conditionChain:条件链作用域不可嵌套");
        }

        // 标签只需在**本次编译的这张图内**唯一:编译器按 tag 分组判定链封闭性,
        // 图之间互不比较。用递增计数器,不用地址 —— 地址会被复用,同一张图里
        // 两条先后开闭的链可能拿到同一个值。
        impl_->active_chain_tag = ++impl_->graph.next_condition_chain_tag;
        impl_->active_condition = std::move(condition);

        return ConditionChainScope{*this, impl_->active_chain_tag};
    }

    RGBuilder::ConditionChainScope::~ConditionChainScope()
    {
        if (owner_ == nullptr)
            return;
        owner_->impl_->active_chain_tag = 0;
        owner_->impl_->active_condition = {};
    }

    RGBuilder::ConditionChainScope::ConditionChainScope(ConditionChainScope&& other) noexcept
        : owner_(other.owner_), tag_(other.tag_)
    {
        other.owner_ = nullptr;
    }

    RGBuilder::ConditionChainScope&
    RGBuilder::ConditionChainScope::operator=(ConditionChainScope&& other) noexcept
    {
        if (this != &other)
        {
            this->~ConditionChainScope();
            owner_ = other.owner_;
            tag_   = other.tag_;
            other.owner_ = nullptr;
        }
        return *this;
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
            if (tex && tex->dimension == lux::rdesc::ETextureDimension::TEX_2D_ARRAY)
                return tex->array_layers;
        }
        return 1;
    }

    RGPassBuilder& RGPassBuilder::inputRead(RGResourceHandle handle, uint32_t attachment_idx)
    {
        RGPassTextureRef ref{};
        ref.resource = handle;
        ref.role = lux::render::ETextureRole::INPUT_ATTACHMENT;
        ref.usage = ERGResourceUsage::READ;
        ref.input_attachment_index = attachment_idx;
        ref.range.base_mip_level = 0;
        ref.range.level_count = 1;
        ref.range.base_array_layer = 0;
        ref.range.layer_count = resolveLayerCount(*graph_, handle);

        pass().textures.push_back(ref);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::read(RGResourceHandle handle, lux::render::ETextureRole role)
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

    RGPassBuilder& RGPassBuilder::write(RGResourceHandle handle, lux::render::ETextureRole role)
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

    RGPassBuilder& RGPassBuilder::readWrite(RGResourceHandle handle, lux::render::ETextureRole role)
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

    RGPassBuilder& RGPassBuilder::before(std::string_view pass_name)
    {
        pass().before_passes.emplace_back(pass_name);
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

    RGPassBuilder& RGPassBuilder::setCondition(PassConditionFn cond, uint64_t chain_tag)
    {
        pass().condition     = std::move(cond);
        pass().condition_tag = chain_tag;
        return *this;
    }

    RGPassBuilder& RGPassBuilder::setEnabled(bool enabled)
    {
        pass().enabled = enabled;
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
        lux::render::ETextureRole consume_tex_role)
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

    // ── Logical-identity overloads ──────────────────────────────────────────
    //  These only record "which engine set to bind," not a slot number. The
    //  slot number gets resolved in computeDescriptorBindingPlan according to
    //  the slot table of whichever pipeline this pass uses (see the note on
    //  `logical` in RGPassTypes). `slot` is pre-filled with the canonical
    //  number as an initial value: when the pipeline has no reflected slot
    //  table (an old-style pipeline that brings its own layout), resolution
    //  can't produce an answer, so that initial value is kept as-is —
    //  matching pre-migration behavior exactly.

    RGPassBuilder& RGPassBuilder::bindImmutableDS(EDescriptorSetSlot logical, VkDescriptorSet ds)
    {
        PassDSBinding b;
        b.slot          = static_cast<uint32_t>(logical);
        b.logical       = logical;
        b.source        = EDSBindingSource::Immutable;
        b.mode          = EDSBindMode::IMMUTABLE;
        b.immutable_set = ds;
        pass().ds_bindings.push_back(b);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::bindSceneDS(EDescriptorSetSlot logical)
    {
        PassDSBinding b;
        b.slot    = static_cast<uint32_t>(logical);
        b.logical = logical;
        b.source  = EDSBindingSource::Scene;
        b.mode    = EDSBindMode::VERSIONED;
        pass().ds_bindings.push_back(b);
        return *this;
    }

    RGPassBuilder& RGPassBuilder::useEngineSet(
        EDescriptorSetSlot logical,
        RGResourceHandle declared_consume,
        ERGResourceType consume_type,
        lux::render::ETextureRole consume_tex_role)
    {
        PassDSBinding b;
        b.slot             = static_cast<uint32_t>(logical);
        b.logical          = logical;
        b.source           = EDSBindingSource::EngineDomain;
        // 域槽一律强制重绑(见 computeDescriptorBindingPlan 里那段 dedup 事故
        // 说明),PER_FIF 是与之匹配的模式。
        b.mode             = EDSBindMode::PER_FIF;
        b.declared_consume = declared_consume;
        b.consume_type     = consume_type;
        b.consume_tex_role = consume_tex_role;
        pass().ds_bindings.push_back(b);
        return *this;
    }

} // namespace lux::render
