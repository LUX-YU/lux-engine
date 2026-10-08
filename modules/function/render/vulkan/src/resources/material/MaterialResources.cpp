#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <algorithm>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/material/MaterialShaderKey.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp> // ensureGlobalMaterialResources
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp> // getMaterialSetLayout

namespace lux::render
{
    MaterialHandle MaterialResources::allocateGlobalHandle()
    {
        if (!free_handle_indices_.empty())
        {
            const uint32_t index = free_handle_indices_.back();
            free_handle_indices_.pop_back();
            handle_alive_[index] = 1u;
            instance_refcounts_[index] = 0u;
            destroy_requested_[index] = 0u;
            return MaterialHandle{index, handle_generations_[index]};
        }

        const uint32_t index = static_cast<uint32_t>(handle_generations_.size());
        handle_generations_.push_back(1u);
        handle_alive_.push_back(1u);
        instance_refcounts_.push_back(0u);
        destroy_requested_.push_back(0u);
        return MaterialHandle{index, 1u};
    }

    void MaterialResources::releaseGlobalHandle(MaterialHandle h) noexcept
    {
        if (!h.isValid() || h.index >= handle_generations_.size())
            return;
        if (!handle_alive_[h.index] || handle_generations_[h.index] != h.gen)
            return;

        handle_alive_[h.index] = 0u;
        ++handle_generations_[h.index];
        free_handle_indices_.push_back(h.index);
    }

    struct MaterialResources::Backing
    {
        SlicedSSBO<UnlitFamilyGPU> unlit;
        SlicedSSBO<LegacyLitFamilyGPU> legacy_lit;
        SlicedSSBO<PbrFamilyGPU> pbr;
        SlicedSSBO<StylizedFamilyGPU> stylized;
        SlicedSSBO<GraphFamilyGPU> graph;
        // Pool-bound allocations: the borrowed ResourceContext pool outlives this resource.
        std::vector<VkDescriptorSet> sets;
        std::uint32_t texture_representation_index;
    };

    MaterialResources::CreateResult MaterialResources::create(const CreateInfo& info) noexcept
    {
        const bool is_missing_binding =
            !info.texture_sampling_catalog || !info.textures || !info.descriptor_pool || !info.set_layout;
        const bool is_invalid_frames = info.ssbo_config.slices == 0 || info.ssbo_config.slices > kMaxFramesInFlight;
        const bool is_invalid_configuration = is_missing_binding || is_invalid_frames;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidMaterialConfiguration>();
        }
        const auto* bindless = info.texture_sampling_catalog->find(kBindlessTextureSamplingRepresentation);
        if (!bindless)
        {
            return renderFailure<err::memory::InvalidMaterialConfiguration>();
        }
        auto unlit = SlicedSSBO<UnlitFamilyGPU>::create(info.ssbo_config);
        if (!unlit)
        {
            return lux::cxx::unexpected(unlit.error());
        }
        auto legacy_lit = SlicedSSBO<LegacyLitFamilyGPU>::create(info.ssbo_config);
        if (!legacy_lit)
        {
            return lux::cxx::unexpected(legacy_lit.error());
        }
        auto pbr = SlicedSSBO<PbrFamilyGPU>::create(info.ssbo_config);
        if (!pbr)
        {
            return lux::cxx::unexpected(pbr.error());
        }
        auto stylized = SlicedSSBO<StylizedFamilyGPU>::create(info.ssbo_config);
        if (!stylized)
        {
            return lux::cxx::unexpected(stylized.error());
        }
        GpuBufferCreateInfo graph_config = info.ssbo_config;
        graph_config.buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        auto graph = SlicedSSBO<GraphFamilyGPU>::create(graph_config);
        if (!graph)
        {
            return lux::cxx::unexpected(graph.error());
        }
        const auto frames = info.ssbo_config.slices;
        std::vector<VkDescriptorSet> sets(frames);
        const std::vector<VkDescriptorSetLayout> layouts(frames, info.set_layout);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = info.descriptor_pool;
        allocation.descriptorSetCount = frames;
        allocation.pSetLayouts = layouts.data();
        const VkDevice device = info.ssbo_config.device_context->logicalDevice();
        VK_EXPECT(vkAllocateDescriptorSets(device, &allocation, sets.data()));
        Backing backing{
            std::move(*unlit),
            std::move(*legacy_lit),
            std::move(*pbr),
            std::move(*stylized),
            std::move(*graph),
            std::move(sets),
            bindless->representation_index
        };
        return std::unique_ptr<MaterialResources>(new MaterialResources(*info.textures, std::move(backing)));
    }

    MaterialResources::MaterialResources(const TextureResources& textures, Backing&& backing) noexcept
        : unlit_ssbo_(std::move(backing.unlit)), legacy_lit_ssbo_(std::move(backing.legacy_lit)),
          pbr_ssbo_(std::move(backing.pbr)), stylized_ssbo_(std::move(backing.stylized)),
          graph_ssbo_(std::move(backing.graph)), descriptor_sets_(std::move(backing.sets)),
          texture_representation_index_(backing.texture_representation_index), textures_(textures)
    {
        bucket_mgr_.seedFamilyBootstrapBuckets();
        for (std::uint32_t index = 0; index < descriptor_sets_.size(); ++index)
        {
            writeDescriptorsOnSet(index);
        }
    }

    MaterialResources::~MaterialResources() noexcept = default;

    void MaterialResources::writeDescriptorsOnSet(uint32_t set_index) const
    {
        unlit_ssbo_.writeDescriptor(descriptor_sets_[set_index], static_cast<uint32_t>(ELightingTechnique::UNLIT));
        legacy_lit_ssbo_.writeDescriptor(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::LEGACY_LIT)
        );
        pbr_ssbo_.writeDescriptor(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::PBR_METALLIC_ROUGHNESS)
        );
        stylized_ssbo_.writeDescriptor(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::STYLIZED)
        );
        graph_ssbo_.writeDescriptor(descriptor_sets_[set_index], static_cast<uint32_t>(ELightingTechnique::GRAPH));

        // Also write the same batch of descriptors into each active scene's
        // domain set. This resource is global while there is one domain set
        // per scene, so this loops per-target -- every other owner has a
        // single target; only these two globals (Material / Texture) have
        // multiple targets.
        for (const auto& t : domain_targets_)
        {
            VkDescriptorSet dds = t.target.setFor(set_index);
            if (dds == VK_NULL_HANDLE)
                continue;
            const auto at = [&](uint32_t family) { return t.target.binding(family); };
            unlit_ssbo_.writeDescriptor(dds, at(static_cast<uint32_t>(ELightingTechnique::UNLIT)));
            legacy_lit_ssbo_.writeDescriptor(dds, at(static_cast<uint32_t>(ELightingTechnique::LEGACY_LIT)));
            pbr_ssbo_.writeDescriptor(dds, at(static_cast<uint32_t>(ELightingTechnique::PBR_METALLIC_ROUGHNESS)));
            stylized_ssbo_.writeDescriptor(dds, at(static_cast<uint32_t>(ELightingTechnique::STYLIZED)));
            graph_ssbo_.writeDescriptor(dds, at(static_cast<uint32_t>(ELightingTechnique::GRAPH)));
        }
    }

    Expected<void> MaterialResources::addDomainWriteTarget(
        const void* owner,
        std::span<const VkDescriptorSet> sets,
        uint32_t binding_offset
    )
    {
        removeDomainWriteTarget(owner); // 重复登记覆盖前一条(场景重建走的就是这条路)
        DomainTarget t{};
        t.owner = owner;
        if (auto accepted = t.target.set(sets, binding_offset); !accepted)
            return accepted;
        domain_targets_.push_back(std::move(t));

        // 立刻写一次:登记时缓冲通常已经就绪,跳过这次写就得等到下一轮 refresh。
        for (uint32_t i = 0; i < descriptor_sets_.size(); ++i)
            writeDescriptorsOnSet(i);
        return {};
    }

    void MaterialResources::removeDomainWriteTarget(const void* owner) noexcept
    {
        std::erase_if(domain_targets_, [owner](const DomainTarget& t) { return t.owner == owner; });
    }

    void MaterialResources::refreshAllDescriptors(uint32_t slice)
    {
        unlit_ssbo_.writeDescriptorTight(
            descriptor_sets_[current_frame_],
            static_cast<uint32_t>(ELightingTechnique::UNLIT),
            slice
        );
        legacy_lit_ssbo_.writeDescriptorTight(
            descriptor_sets_[current_frame_],
            static_cast<uint32_t>(ELightingTechnique::LEGACY_LIT),
            slice
        );
        pbr_ssbo_.writeDescriptorTight(
            descriptor_sets_[current_frame_],
            static_cast<uint32_t>(ELightingTechnique::PBR_METALLIC_ROUGHNESS),
            slice
        );
        stylized_ssbo_.writeDescriptorTight(
            descriptor_sets_[current_frame_],
            static_cast<uint32_t>(ELightingTechnique::STYLIZED),
            slice
        );
        graph_ssbo_.writeDescriptorTight(
            descriptor_sets_[current_frame_],
            static_cast<uint32_t>(ELightingTechnique::GRAPH),
            slice
        );
    }

    void MaterialResources::refreshAllDescriptorsOnSet(uint32_t set_index, uint32_t slice)
    {
        unlit_ssbo_
            .writeDescriptorTight(descriptor_sets_[set_index], static_cast<uint32_t>(ELightingTechnique::UNLIT), slice);
        legacy_lit_ssbo_.writeDescriptorTight(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::LEGACY_LIT),
            slice
        );
        pbr_ssbo_.writeDescriptorTight(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::PBR_METALLIC_ROUGHNESS),
            slice
        );
        stylized_ssbo_.writeDescriptorTight(
            descriptor_sets_[set_index],
            static_cast<uint32_t>(ELightingTechnique::STYLIZED),
            slice
        );
        graph_ssbo_
            .writeDescriptorTight(descriptor_sets_[set_index], static_cast<uint32_t>(ELightingTechnique::GRAPH), slice);
    }

    // (MaterialResources::submit(rdesc::Material) — the builtin closure-material upload
    //  path — retired in W5a. Graph materials use submitGraph() below.)

    // Pack a flat GraphMaterialData blob into the GPU struct (shared by
    // submitGraph + modifyGraph). Graph materials are data-driven — no
    // rdesc::Material variant — so this is a separate path from submit().
    bool MaterialResources::packGraphGpu(const GraphMaterialData& data, GraphFamilyGPU& gpu) const
    {
        gpu.shading_model_id = static_cast<uint32_t>(EShadingModel::GRAPH);
        gpu.feature_mask = 0u;
        gpu.tex_mask = data.tex_mask;
        gpu.flags = data.flags;

        const uint32_t np = std::min<uint32_t>(data.param_count, GraphMaterialData::kMaxParams);
        for (uint32_t i = 0; i < np; ++i)
            gpu.params[i] = {data.params[i][0], data.params[i][1], data.params[i][2], data.params[i][3]};
        for (uint32_t i = 0; i < GraphMaterialData::kMaxTextures; ++i)
        {
            gpu.tex[i].representation_index = texture_representation_index_;
            if ((data.tex_mask & (1u << i)) == 0)
                continue;
            const auto local = textures_.resolveTexture(data.textures[i]);
            if (!local.isValid())
                return false;
            gpu.tex[i].resource_index = local.index;
        }
        return true;
    }

    Expected<MaterialHandle> MaterialResources::submitGraph(
        const GraphMaterialData& data,
        ShaderHandle gbuffer_shader,
        ShaderHandle forward_shader,
        uint64_t shader_key,
        lux::rdesc::EAlphaMode alpha_mode,
        bool double_sided
    )
    {
        ds_revision_.bump();

        GraphFamilyGPU gpu{};
        if (!packGraphGpu(data, gpu))
            return renderFailure<err::resource::NotFound>();
        // double_sided also rides the GPU flags (EMaterialGpuFlag::DOUBLE_SIDED) for any
        // shader-side two-sided handling; the load-bearing effect is the cull tier.
        if (double_sided)
            gpu.flags |= static_cast<uint32_t>(EMaterialGpuFlag::DOUBLE_SIDED);
        const SlotHandle local_slot = graph_ssbo_.add(gpu);

        const MaterialHandle ret = allocateGlobalHandle();
        // A graph material with its own baked shader gets its OWN bucket/PSO (R1);
        // without one (legacy callers / shader_key == 0) it shares the Graph family
        // bucket — the pre-R1 behavior. W3a: render-state (alpha_mode/double_sided)
        // folds into the graph bucket key so distinct render-state -> distinct PSO.
        const uint32_t bucket =
            (shader_key != 0)
                ? bucket_mgr_.getOrCreateGraph(shader_key, gbuffer_shader, forward_shader, alpha_mode, double_sided)
                : bucket_mgr_.getOrCreate(ELightingTechnique::GRAPH, 0u);
        slot_records_.insert(
            ret,
            SlotRecord{
                .family = ELightingTechnique::GRAPH,
                .shading_model = EShadingModel::GRAPH,
                .local_slot = local_slot,
                .feature_mask = 0u,
                .variant_bucket = bucket,
                .packed_material_type = packMaterialType(EShadingModel::GRAPH),
            }
        );
        return ret;
    }

    RenderError MaterialResources::modifyGraph(MaterialHandle slot, const GraphMaterialData& data)
    {
        auto* rec = slot_records_.find(slot);
        if (!rec)
            return renderError<err::resource::NotFound>();
        if (rec->family != ELightingTechnique::GRAPH)
            return renderError<err::resource::TypeMismatch>();

        GraphFamilyGPU gpu{};
        if (!packGraphGpu(data, gpu))
            return renderError<err::resource::NotFound>();
        // In-place data update at the same slot — no descriptor/graph change.
        const bool ok = graph_ssbo_.modify(rec->local_slot, gpu);
        return ok ? RenderError{} : renderError<err::resource::ModifyFailed>();
    }

    bool MaterialResources::retainForInstance(MaterialHandle slot) noexcept
    {
        if (!slot_records_.find(slot) || slot.index >= destroy_requested_.size() ||
            destroy_requested_[slot.index] != 0u)
        {
            return false;
        }
        ++instance_refcounts_[slot.index];
        return true;
    }

    void MaterialResources::releaseFromInstance(MaterialHandle slot) noexcept
    {
        if (!slot_records_.find(slot) || slot.index >= instance_refcounts_.size() ||
            instance_refcounts_[slot.index] == 0u)
        {
            return;
        }
        --instance_refcounts_[slot.index];
        if (instance_refcounts_[slot.index] == 0u && destroy_requested_[slot.index] != 0u)
        {
            removeNow(slot);
        }
    }

    void MaterialResources::remove(MaterialHandle slot)
    {
        if (!slot_records_.find(slot))
            return;
        if (instance_refcounts_[slot.index] != 0u)
        {
            destroy_requested_[slot.index] = 1u;
            return;
        }
        removeNow(slot);
    }

    void MaterialResources::removeNow(MaterialHandle slot)
    {
        auto* rec = slot_records_.find(slot);
        if (!rec)
            return;

        switch (rec->family)
        {
        case ELightingTechnique::UNLIT:
            unlit_ssbo_.remove(rec->local_slot);
            break;
        case ELightingTechnique::LEGACY_LIT:
            legacy_lit_ssbo_.remove(rec->local_slot);
            break;
        case ELightingTechnique::PBR_METALLIC_ROUGHNESS:
            pbr_ssbo_.remove(rec->local_slot);
            break;
        case ELightingTechnique::STYLIZED:
            stylized_ssbo_.remove(rec->local_slot);
            break;
        case ELightingTechnique::GRAPH:
            graph_ssbo_.remove(rec->local_slot);
            break;
        default:
            break;
        }

        // Release this material's reference to its variant bucket so destroyed
        // materials no longer pin a bucket forever (the old leak that drove the
        // 128 soft cap and its silent Unlit-degrade). No-op for family/bootstrap
        // buckets (e.g. the shader_key==0 fallback). handleDestroyMaterial
        // invalidates all scene graphs right after this, so a freed+reused bucket
        // id is rebound to its new PSO before any instance references it.
        bucket_mgr_.release(rec->variant_bucket);

        slot_records_.erase(slot);
        instance_refcounts_[slot.index] = 0u;
        destroy_requested_[slot.index] = 0u;
        releaseGlobalHandle(slot);
    }

    // (MaterialResources::modify(rdesc::Material) — the builtin closure-material modify
    //  path — retired in W5a. Graph materials use modifyGraph() / submitGraph().)

    void MaterialResources::uploadData(VkCommandBuffer cb, const FrameStamp& stamp)
    {
        const uint32_t frame_index = stamp.slotIndex();
        current_frame_ = frame_index;

        if (ds_revision_.needsWrite(frame_index))
        {
            refreshDescriptors(frame_index);
        }

        // Upload data for all 5 family SSBOs
        unlit_ssbo_.uploadDataSlice(cb, frame_index);
        legacy_lit_ssbo_.uploadDataSlice(cb, frame_index);
        pbr_ssbo_.uploadDataSlice(cb, frame_index);
        stylized_ssbo_.uploadDataSlice(cb, frame_index);
        graph_ssbo_.uploadDataSlice(cb, frame_index);
    }

    // ── 全局材质栈的惰性构造器 ──────────────────────────────────────────
    //
    // 从 L6 的 assembly/material/MaterialOperationHandlers.cpp 搬来 —— 归位理由
    // 同 ensureGlobalMeshResources(见 MeshResources.cpp)。
    Expected<void> ensureGlobalMaterialResources(RenderContext& ctx)
    {
        auto& greg = ctx.globalRegistry();
        if (greg.find<MaterialResources>())
            return {};

        MaterialResources::CreateInfo info{};
        info.ssbo_config = SSBOInitConfig{
            .device_context = &ctx.deviceContext(),
            .deferred_queue = &ctx.deferredDestroyQueue(),
            .initial_dense_capacity = 512,
            .slices = ctx.framesInFlight(),
        };
        info.descriptor_pool = ctx.resourceContext().descriptorPool();
        info.set_layout = ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::MATERIAL);
        info.texture_sampling_catalog = &ctx.textureSamplingRepresentations();
        info.textures = &ctx.globalRegistry().must<TextureResources>();
        auto candidate = MaterialResources::create(info);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        auto* mat = greg.insert(std::move(*candidate)).get();

        // 每帧维护由安装点在完整对象发布后登记;失败候选不得留下回调。
        greg.addBeginFrameHook(EUploadPhase::UPLOAD, [mat](const FrameStamp& s) { mat->onFrameBeginMaintenance(s); });
        ctx.globalTransferScheduler().contributors().add(makeTransferContributor(mat, /*priority=*/5));
        return {};
    }

} // namespace lux::render
