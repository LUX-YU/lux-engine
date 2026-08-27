#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutationCompiler.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp> // toVkDescriptorType / engineShapeBindingFor
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/LayoutContract.hpp>

#include <cassert>
#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <map>

namespace lux::render
{
    // Helper: Convert lux::rdesc::EShaderType to VkShaderStageFlags
    static VkShaderStageFlags getVkShaderStage(rdesc::EShaderType type)
    {
        switch (type)
        {
        case rdesc::EShaderType::VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case rdesc::EShaderType::FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case rdesc::EShaderType::GEOMETRY:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case rdesc::EShaderType::COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case rdesc::EShaderType::TESSELLATION_CTRL:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case rdesc::EShaderType::TESSELLATION_EVAL:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case rdesc::EShaderType::RAYGEN:
            return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        case rdesc::EShaderType::ANY_HIT:
            return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        case rdesc::EShaderType::CLOSEST_HIT:
            return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        case rdesc::EShaderType::MISS:
            return VK_SHADER_STAGE_MISS_BIT_KHR;
        case rdesc::EShaderType::INTERSECTION:
            return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
        case rdesc::EShaderType::CALLABLE:
            return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        default:
            return 0;
        }
    }

    // toVkDescriptorType is provided by EngineSetShapes.hpp (the shared
    // constexpr version, which also drives the compile-time cross-check
    // between the contract and the shape table).

    // EngineSetShapes.hpp 提供跨 stage 合并状态一致性所需的判定(与
    // check used for switch-over detection).

    static PipelineReflectedInfo mergeShaderInfos(std::span<const rdesc::ShaderInfo* const> shaders)
    {
        PipelineReflectedInfo result;
        bool any_merged = false;
        bool unmerged_movable = false;

        // Temporary storage for Push Constants: offset -> range
        // Used to merge stageFlags for ranges with the same offset
        struct PCRange
        {
            uint32_t size;
            VkShaderStageFlags stages;
        };
        std::map<uint32_t, PCRange> pc_map;

        for (const auto* shader : shaders)
        {
            if (!shader)
                continue;

            if (shader->merged_domain_layout)
                any_merged = true;
            else if (hasNonIdentityRelocation(*shader))
                unmerged_movable = true;

            // Union of the private remap tables (used to translate bare-slot
            // bindings); the same `from` mapping to a different `to` means
            // the stages disagree on the assignment, and is treated as a
            // conflict.
            for (const auto& [from, to] : shader->private_set_remap)
            {
                bool seen = false;
                for (const auto& [f, t] : result.private_set_remap)
                    if (f == from)
                    {
                        seen = true;
                        if (t != to)
                            unmerged_movable = true;
                        break;
                    }
                if (!seen)
                    result.private_set_remap.push_back({from, to});
            }

            // Deduce the Stage of the current Shader
            VkShaderStageFlags stage_flags = 0;
            if (!shader->entry_points.empty())
            {
                stage_flags = getVkShaderStage(shader->entry_points[0].stage);
            }

            // 1. Merge Descriptor Sets — mask + full per-binding detail.
            //    Reflection is no longer discarded: the same (set, binding)
            //    across stages has its stageFlags merged, and type/count must
            //    agree (a mismatch between shaders is exactly what triggers
            //    the registration-time error below).
            for (const auto& set_info : shader->sets)
            {
                // If this Set appears in any Shader, mark it as active
                result.active_sets_mask |= (1u << set_info.set);

                ReflectedSet* rset = nullptr;
                for (auto& existing : result.reflected_sets)
                    if (existing.set == set_info.set)
                    {
                        rset = &existing;
                        break;
                    }
                if (!rset)
                {
                    result.reflected_sets.push_back({set_info.set, {}});
                    rset = &result.reflected_sets.back();
                }
                for (const auto& b : set_info.bindings)
                {
                    ReflectedSetBinding* rb = nullptr;
                    for (auto& existing : rset->bindings)
                        if (existing.binding == b.binding)
                        {
                            rb = &existing;
                            break;
                        }
                    if (rb)
                    {
                        rb->stages |= stage_flags;
                        assert(
                            rb->type == toVkDescriptorType(b.type) &&
                            "mergeShaderInfos: same (set,binding) with different type across stages");
                    }
                    else
                    {
                        rset->bindings.push_back(
                            {b.binding, toVkDescriptorType(b.type), std::max(b.count, 1u), stage_flags, b.name}
                        );
                    }
                }
            }

            // 2. Merge Push Constants
            for (const auto& pc : shader->push_constants)
            {
                auto it = pc_map.find(pc.offset);
                if (it == pc_map.end())
                {
                    pc_map[pc.offset] = {pc.size, stage_flags};
                }
                else
                {
                    // Simple merge check: if offset is the same, size should be the same (or a union)
                    // And accumulate stageFlags
                    it->second.stages |= stage_flags;
                    it->second.size = std::max(it->second.size, pc.size);
                }
            }
        }

        // Convert Push Constants to Vulkan format
        for (const auto& [offset, range] : pc_map)
        {
            VkPushConstantRange vk_range{};
            vk_range.stageFlags = range.stages;
            vk_range.offset = offset;
            vk_range.size = range.size;
            result.push_constant_ranges.push_back(vk_range);
        }

        result.merged_domain_layout = any_merged;
        result.merged_flag_conflict = any_merged && unmerged_movable;
        return result;
    }

    // (RenderPassKey::is_compatible_with 已删:零调用点。它也不是"兼容"判定
    //  ——depth 为 UNDEFINED 时直接返回 false,即"无深度的两个键互不兼容",
    //  与名字相反。渲染通道复用现役走 render_pass_cache_ 的精确相等键。)

    PipelineManager::PipelineManager(DeviceContext& device_ctx, bool use_dynamic_rendering)
        : device_ctx_(&device_ctx), use_dynamic_rendering_(use_dynamic_rendering)
    {
    }

    PipelineManager::~PipelineManager()
    {
        destroyAll();
    }

    void PipelineManager::destroyAll()
    {
        // Destroy pipelines first
        for (auto& kv : pipeline_cache_)
        {
            if (kv.second.pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(device_ctx_->logicalDevice(), kv.second.pipeline, nullptr);
                kv.second.pipeline = VK_NULL_HANDLE;
            }
        }
        pipeline_cache_.clear();

        // Destroy compute pipelines
        for (auto& rec : compute_pipelines_)
        {
            if (rec.pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(device_ctx_->logicalDevice(), rec.pipeline, nullptr);
                rec.pipeline = VK_NULL_HANDLE;
            }
        }
        compute_pipelines_.clear();
        compute_set_layouts_.clear();
        compute_reflections_.clear();

        pipeline_templates_.clear();
        template_set_layouts_.clear();
        template_reflections_.clear();
        template_layout_epochs_.clear();
        template_variant_masks_.clear();
        template_variant_fallback_counts_.clear();

        // Then destroy render passes
        for (auto& kv : render_pass_cache_)
        {
            if (kv.second != VK_NULL_HANDLE)
            {
                vkDestroyRenderPass(device_ctx_->logicalDevice(), kv.second, nullptr);
                kv.second = VK_NULL_HANDLE;
            }
        }
        render_pass_cache_.clear();
    }

    // -------------------------------------------------------------------------
    // Compute pipeline management
    // -------------------------------------------------------------------------

    Expected<ComputePipelineHandle> PipelineManager::registerComputePipelineReflected(
        VkShaderModule shader,
        const rdesc::ShaderInfo& info,
        std::string debug_name,
        std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specialization_values,
        std::span<const std::pair<uint32_t, VkDescriptorSetLayout>> explicit_set_layouts
    )
    {
        PipelineReflectedInfo reflected = mergeShaderInfos(std::array{&info});

        // Compute has no slot-semantics holes; the hint only carries the
        // debug name and any explicit shared-set overrides.
        GraphicsPipelineTemplate hint{};
        hint.debug_name = std::move(debug_name);
        for (const auto& e : explicit_set_layouts)
            hint.explicit_set_layouts.push_back(e);

        lux::cxx::SmallVector<VkDescriptorSetLayout, 8> set_layouts;
        auto layout = buildReflectedPipelineLayout(reflected, hint, set_layouts);
        if (!layout)
            return lux::cxx::unexpected(layout.error());

        const ComputePipelineHandle handle = registerComputePipeline(shader, *layout, specialization_values);
        // registerComputePipeline appended an empty table / empty reflection
        // for this handle — replace them with what was just built.
        compute_set_layouts_.back() = std::move(set_layouts);
        compute_reflections_.back() = reflected;
        return handle;
    }

    VkDescriptorSetLayout PipelineManager::computeSetLayout(ComputePipelineHandle handle, uint32_t set) const noexcept
    {
        if (handle.index >= compute_set_layouts_.size())
            return VK_NULL_HANDLE;
        const auto& layouts = compute_set_layouts_[handle.index];
        return set < layouts.size() ? layouts[set] : VK_NULL_HANDLE;
    }

    ComputePipelineHandle PipelineManager::registerComputePipeline(
        VkShaderModule shader,
        VkPipelineLayout layout,
        std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specialization_values
    )
    {
        const ComputePipelineHandle handle{static_cast<uint32_t>(compute_pipelines_.size())};
        compute_set_layouts_.emplace_back();
        compute_reflections_.emplace_back();

        std::vector<VkSpecializationMapEntry> spec_entries;
        std::vector<uint32_t> spec_values;
        spec_entries.reserve(specialization_values.size());
        spec_values.reserve(specialization_values.size());

        for (const auto& sv : specialization_values)
        {
            if (sv.stage != VK_SHADER_STAGE_COMPUTE_BIT)
                continue;

            VkSpecializationMapEntry entry{};
            entry.constantID = sv.constant_id;
            entry.offset = static_cast<uint32_t>(spec_values.size() * sizeof(uint32_t));
            entry.size = sizeof(uint32_t);
            spec_entries.push_back(entry);
            spec_values.push_back(sv.value);
        }

        VkSpecializationInfo spec_info{};
        if (!spec_entries.empty())
        {
            spec_info.mapEntryCount = static_cast<uint32_t>(spec_entries.size());
            spec_info.pMapEntries = spec_entries.data();
            spec_info.dataSize = spec_values.size() * sizeof(uint32_t);
            spec_info.pData = spec_values.data();
        }

        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = layout;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader;
        ci.stage.pName = "main";
        ci.stage.pSpecializationInfo = spec_entries.empty() ? nullptr : &spec_info;

        VkPipeline vk_pipeline = VK_NULL_HANDLE;
        const auto create_started = std::chrono::steady_clock::now();
        const VkResult res =
            vkCreateComputePipelines(device_ctx_->logicalDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &vk_pipeline);
        const auto create_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_started)
                .count()
        );
        ++telemetry_.compute_create_calls;
        telemetry_.compute_create_nanoseconds += create_nanoseconds;
        telemetry_.compute_create_max_nanoseconds =
            std::max(telemetry_.compute_create_max_nanoseconds, create_nanoseconds);
        if (res != VK_SUCCESS)
        {
            ++telemetry_.compute_create_failures;
            // Do not consume a pipeline slot on failure. Returning an
            // index-valid handle wrapping VK_NULL_HANDLE would make
            // ComputePipelineHandle::valid() report true and silently feed a
            // null pipeline into vkCmdBindPipeline/vkCmdDispatch. Return the
            // sentinel instead so callers' .valid() guards (and the RG
            // compile-time null check) detect the failure and can retry later.
            if (error_sink_ != nullptr)
                error_sink_->emit(
                    renderError<err::device::VulkanCallFailed>(encodeVkResult(res)),
                    RenderErrorEvent::kNoScene,
                    0
                );
            return kInvalidComputePipelineHandle;
        }

        compute_pipelines_.push_back({vk_pipeline, layout});
        return handle;
    }

    VkPipeline PipelineManager::getComputePipeline(ComputePipelineHandle handle) const noexcept
    {
        if (!handle.valid() || handle.index >= compute_pipelines_.size())
            return VK_NULL_HANDLE;
        return compute_pipelines_[handle.index].pipeline;
    }

    VkPipelineLayout PipelineManager::getComputeLayout(ComputePipelineHandle handle) const noexcept
    {
        if (!handle.valid() || handle.index >= compute_pipelines_.size())
            return VK_NULL_HANDLE;
        return compute_pipelines_[handle.index].layout;
    }

    // -------------------------------------------------------------------------
    // Graphics pipeline templates
    // -------------------------------------------------------------------------

    void PipelineManager::setReflectedLayoutEnv(
        GeneralDescriptorSetLayout& shared,
        DescriptorService& descriptors,
        PipelineLayoutService& pipeline_layouts
    ) noexcept
    {
        shared_layouts_ = &shared;
        descriptor_service_ = &descriptors;
        pipeline_layout_service_ = &pipeline_layouts;
    }

    VkDescriptorSetLayout PipelineManager::templateSetLayout(GraphicsPipelineHandle handle, uint32_t set) const noexcept
    {
        if (handle.index >= template_set_layouts_.size())
            return VK_NULL_HANDLE;
        const auto& layouts = template_set_layouts_[handle.index];
        return set < layouts.size() ? layouts[set] : VK_NULL_HANDLE;
    }

    ResolvedPrivateSetLayout
    PipelineManager::templatePrivateSetLayout(GraphicsPipelineHandle handle, uint32_t source_set) const noexcept
    {
        ResolvedPrivateSetLayout result{source_set, source_set, VK_NULL_HANDLE};
        const auto* reflection = templateReflection(handle);
        if (!reflection)
            return result;

        for (const auto& [from, to] : reflection->private_set_remap)
        {
            if (from == source_set)
            {
                result.runtime_slot = to;
                break;
            }
        }

        for (const auto& slot : reflection->slots)
        {
            if (slot.slot == result.runtime_slot && slot.source == ESlotSource::PipelinePrivate)
            {
                result.layout = slot.layout;
                break;
            }
        }
        return result;
    }

    Expected<void> PipelineManager::finalizeTemplateLayout(
        GraphicsPipelineHandle handle,
        VkPipelineLayout layout,
        std::span<const VkDescriptorSetLayout> set_layouts
    )
    {
        if (handle.index >= pipeline_templates_.size() || layout == VK_NULL_HANDLE)
            return renderFailure<err::internal::InvalidArgument>();

        pipeline_templates_[handle.index].pipeline_layout = layout;
        template_set_layouts_[handle.index].assign(set_layouts.begin(), set_layouts.end());
        ++template_layout_epochs_[handle.index];
        return {};
    }

    Expected<bool> PipelineManager::rebuildTemplateLayout(
        GraphicsPipelineHandle handle,
        std::span<const VkDescriptorSetLayout> set_layouts
    )
    {
        if (handle.index >= pipeline_templates_.size() || !pipeline_layout_service_)
            return renderFailure<err::internal::InvalidArgument>();

        // Identical to the existing layout -> return immediately without
        // touching the epoch (otherwise every graph recompile would
        // invalidate the entire PSO cache and trigger an avalanche of
        // rebuilds).
        const auto& current = template_set_layouts_[handle.index];
        if (std::equal(current.begin(), current.end(), set_layouts.begin(), set_layouts.end()))
            return false;

        const auto& refl = template_reflections_[handle.index];
        std::vector<VkPushConstantRange> pc;
        if (refl.has_value())
            pc.assign(refl->push_constant_ranges.begin(), refl->push_constant_ranges.end());

        auto layout = pipeline_layout_service_->getOrCreate({
            .set_layouts = set_layouts,
            .push_constants = pc,
            .debug_name = pipeline_templates_[handle.index].debug_name,
        }
        );
        if (!layout)
            return lux::cxx::unexpected(layout.error());

        auto applied = finalizeTemplateLayout(handle, *layout, set_layouts);
        if (!applied)
            return lux::cxx::unexpected(applied.error());
        return true;
    }

    uint32_t PipelineManager::maxBoundDescriptorSets() const noexcept
    {
        return pipeline_layout_service_ ? pipeline_layout_service_->maxBoundDescriptorSets() : 0u;
    }

    uint32_t PipelineManager::templateLayoutEpoch(GraphicsPipelineHandle handle) const noexcept
    {
        return handle.index < template_layout_epochs_.size() ? template_layout_epochs_[handle.index] : 0u;
    }

    const PipelineReflectedInfo* PipelineManager::templateReflection(GraphicsPipelineHandle handle) const noexcept
    {
        if (handle.index >= template_reflections_.size())
            return nullptr;
        const auto& r = template_reflections_[handle.index];
        return r.has_value() ? &*r : nullptr;
    }

    const PipelineReflectedInfo* PipelineManager::computeReflection(ComputePipelineHandle handle) const noexcept
    {
        if (handle.index >= compute_reflections_.size())
            return nullptr;
        const auto& r = compute_reflections_[handle.index];
        return r.has_value() ? &*r : nullptr;
    }

    Expected<VkPipelineLayout> PipelineManager::buildReflectedPipelineLayout(
        PipelineReflectedInfo& reflected,
        const GraphicsPipelineTemplate& description,
        lux::cxx::SmallVector<VkDescriptorSetLayout, 8>& out_set_layouts
    )
    {
        if (!shared_layouts_ || !descriptor_service_ || !pipeline_layout_service_)
            return renderFailure<err::internal::InvalidArgument>();

        const std::string& debug_name = description.debug_name;

        // 跨 stage 的合并标记冲突(一个 stage 已为域合并搬运,另一个还带着会移动的引擎
        // 资源没搬)意味着两侧的位置根本对不上 —— 在注册期失败,而不是留到录制期。
        if (reflected.merged_flag_conflict)
            return renderFailure<err::pipeline::StageMergeStateConflict>();

        // The set array must be contiguous from 0 up to the highest slot.
        // The upper bound considers both reflection and the caller's
        // resource_slot_map (a set the pipeline conventionally binds may not
        // be referenced by the shader — e.g. Tonemap binds Scene but its
        // fragment shader doesn't read it, so reflection can't see it).
        uint32_t max_set = 0;
        for (const auto& rs : reflected.reflected_sets)
            max_set = std::max(max_set, rs.set);
        for (const auto& [slot, index] : description.resource_slot_map)
            max_set = std::max(max_set, index);

        // Explicit overrides also count toward the upper bound (some shared
        // sets may not be referenced by this pipeline's shader).
        for (const auto& [index, layout] : description.explicit_set_layouts)
            max_set = std::max(max_set, index);

        out_set_layouts.clear();
        reflected.slots.clear();
        // Record the classification per slot, growing in lockstep with
        // out_set_layouts (the index IS the slot number).
        const auto noteSlot = [&](uint32_t slot, ESlotSource src, uint32_t logical, VkDescriptorSetLayout layout) {
            reflected.slots.push_back(ReflectedSlot{slot, src, logical, layout});
        };

        for (uint32_t s = 0; s <= max_set; ++s)
        {
            // 1) A shared set explicitly declared by a feature takes highest
            //    priority (see the comment on explicit_set_layouts).
            VkDescriptorSetLayout explicit_layout = VK_NULL_HANDLE;
            for (const auto& [index, layout] : description.explicit_set_layouts)
                if (index == s)
                {
                    explicit_layout = layout;
                    break;
                }
            if (explicit_layout != VK_NULL_HANDLE)
            {
                out_set_layouts.push_back(explicit_layout);
                noteSlot(s, ESlotSource::FeatureExplicit, s, explicit_layout);
                continue;
            }

            const ReflectedSet* rset = nullptr;
            for (const auto& rs : reflected.reflected_sets)
                if (rs.set == s)
                {
                    rset = &rs;
                    break;
                }

            if (!rset)
            {
                // A reflection hole means the shader doesn't reference this
                // set, but at record time the engine's usual convention may
                // still bind a shared set into this slot (e.g. Skybox doesn't
                // read Instance, but it still gets bound at record time). An
                // empty layout would make vkCmdBindDescriptorSets fail with
                // 00358 (0 descriptors expected vs. N actually bound), so a
                // hole is always filled with THAT SLOT'S ENGINE-SHARED
                // LAYOUT: it shares the same source as the set that gets
                // bound in, so it's always compatible; if the caller's
                // resource_slot_map declares semantics for this slot, prefer
                // that (the two agree under the standard layout anyway).
                //
                // DOMAIN-MODE EXCEPTION: in a merged pipeline the slot number
                // is a domain slot and no longer equals the canonical number
                // — tagging a hole with engine semantics via "slot number ==
                // semantics" would make record time bind the old canonical
                // set into the domain slot (the same mistake repeated). A
                // domain-mode hole is therefore always a SEMANTICS-FREE
                // placeholder: logical uses the kDescriptorSetCount sentinel
                // (which the slot-table synthesis step skips).
                //
                // But the placeholder layout must still be looked up by
                // DOMAIN-SLOT SEMANTICS, sharing the same source as the real
                // path: within the same pass, "the primary pipeline that uses
                // this slot" and "a variant that doesn't" must get the exact
                // same layout handle, otherwise the variant-compatibility
                // chain breaks right at the hole (caught in practice by the
                // editor's gate: the unlit primary pipeline has set 1 =
                // Texture layout, the pbr variant's set 1 is a hole; if the
                // hole resolved by slot number it would get the Instance
                // layout, producing VUID-08600 at record time). This is
                // exactly the domain-mode counterpart of the unmerged path's
                // rule "fill a hole with the shared layout for its slot
                // semantics".
                const EDescriptorSetSlot* hinted = nullptr;
                for (const auto& [slot, index] : description.resource_slot_map)
                    if (index == s)
                    {
                        hinted = &slot;
                        break;
                    }

                const bool merged = reflected.merged_domain_layout;
                VkDescriptorSetLayout hole_layout = VK_NULL_HANDLE;
                uint32_t canonical = 0;
                if (merged)
                {
                    canonical = kDescriptorSetCount; // semantics-free sentinel
                    if (s == domainSetSlot(rdesc::EBindFrequency::GLOBAL))
                        hole_layout = shared_layouts_->getDomainLayout(rdesc::EBindFrequency::GLOBAL);
                    else if (s == domainSetSlot(rdesc::EBindFrequency::BINDLESS))
                        hole_layout = shared_layouts_->getDomainLayout(rdesc::EBindFrequency::BINDLESS);
                    else if (s == domainSetSlot(rdesc::EBindFrequency::FEATURE))
                        hole_layout = shared_layouts_->getDomainLayout(rdesc::EBindFrequency::FEATURE);
                    else
                        // PASS_LOCAL slots and above: a pure placeholder —
                        // the primary pipeline and its variants both follow
                        // this same rule, so they necessarily agree.
                        hole_layout = shared_layouts_->getLayout(std::min(s, kDescriptorSetCount - 1u));
                }
                else
                {
                    // 非合并管线的 canonical hole 回填 —— **这条不是 legacy,
                    // 不随域合并退休**:树外无引擎成员的管线(GaussianSplat
                    // 等)合法地按 canonical 字面量槽位布局,set 空洞要有占位
                    // 布局才能建 pipeline layout。审计期(ef27b6c)的埋点已
                    // 撤除:它对树外管线是常态,报出来只是噪音。
                    canonical = hinted ? static_cast<uint32_t>(*hinted) : s;
                    hole_layout = shared_layouts_->getLayout(std::min(canonical, kDescriptorSetCount - 1u));
                }
                out_set_layouts.push_back(hole_layout);
                noteSlot(s, ESlotSource::ReflectionHole, canonical, hole_layout);
                continue;
            }

            // Routing: if any binding in the set hits a contract engine_set,
            // take the layout from the engine-shared table. THE CRITERION IS
            // OWNERSHIP, NOT FREQUENCY DOMAIN: these sets' instances are
            // allocated with the full shape by the engine resource object
            // (Instance/Light/Material/Texture/VertexPool/...), while
            // reflection only sees the subset of bindings this pipeline
            // actually uses — building a layout from the subset triggers
            // VUID-00358 (e.g. the shadow caster vert only reads uTransforms,
            // not uProperties, but the Instance set bound in has both).
            const rdesc::LogicalResourceDesc* owned = nullptr;
            for (const auto& b : rset->bindings)
                if (const auto* e = rdesc::findLogicalResource(b.name); e && e->engine_set)
                {
                    owned = e;
                    break;
                }

            if (owned && reflected.merged_domain_layout)
            {
                // ── 域模式路由:本管线的引擎资源已经为域合并搬运过 ─────────
                //
                // 槽位号与每个 binding 的位置都是搬运表(名字 → 契约 → 域内偏移)的
                // 确定性函数。SPIR-V、反射、布局三者必须指向同一个位置;任一方落后都在
                // 这里拒绝,而不是留到录制期以 VUID 的形式冒出来。
                //
                // 错误实参只带**实际值**与契约索引 —— 期望值是契约的纯函数,消费侧拿着
                // 索引自己就能算出「本该在哪」。
                const auto domain = kEngineSetShapes[owned->canonical_set].frequency;

                if (s != domainSetSlot(domain))
                    return renderFailure<err::pipeline::DomainSlotMismatch>(
                        rdesc::logicalResourceIndex(owned->name),
                        static_cast<std::uint32_t>(domain),
                        s
                    );

                for (const auto& b : rset->bindings)
                {
                    const auto* entry = engineOwnedResource(b.name);
                    if (!entry)
                        return renderFailure<err::pipeline::BindingNotInContract>(s, b.binding);

                    const auto binding_domain = kEngineSetShapes[entry->canonical_set].frequency;
                    const uint32_t want_binding =
                        engineSetDomainOffset(entry->canonical_set) + entry->canonical_binding;
                    if (domainSetSlot(binding_domain) != s || b.binding != want_binding)
                        return renderFailure<err::pipeline::BindingPositionMismatch>(
                            rdesc::logicalResourceIndex(b.name),
                            s,
                            b.binding
                        );

                    const auto* shape = engineShapeBindingFor(entry->canonical_set, entry->canonical_binding);
                    if (!shape || shape->type != b.type)
                        return renderFailure<err::pipeline::BindingTypeMismatch>(rdesc::logicalResourceIndex(b.name));
                }

                // Every domain uniformly takes the domain layout + the
                // DomainMerged classification (this also sets up the eventual
                // retirement of the canonical path, since it makes
                // getLayout(canonical) see zero use on merged pipelines).
                // logical_set stores the domain enum value; member identity
                // is looked up downstream from the reflected name via the
                // contract.
                //
                // At record time the instance is dispatched by domain (see
                // the collapsing branch of the binding plan): FEATURE/GLOBAL
                // bind the domain instance from SceneDomainDescriptorSets (a
                // dual write keeps it consistent with the per-set data);
                // BINDLESS's domain "instance" is simply the global texture
                // table itself (by design, it isn't one of the per-scene
                // domain instances), so it keeps whatever global set the
                // binding already carries — the domain layout and its
                // allocated layout are identically defined, so they're
                // compatible.
                VkDescriptorSetLayout dl = shared_layouts_->getDomainLayout(domain);
                if (dl == VK_NULL_HANDLE)
                    return renderFailure<err::pipeline::DomainLayoutNotInitialised>(static_cast<std::uint32_t>(domain));
                out_set_layouts.push_back(dl);
                noteSlot(s, ESlotSource::DomainMerged, static_cast<uint32_t>(domain), dl);
                continue;
            }

            if (owned)
            {
                // 引用引擎契约资源的管线**必须**走上面那个域模式分支。走到这里只有两种
                // 成因,都是错误:某个 stage 没过切换入口(多 stage 合并时丢了域合并
                // 标记),或者着色器引用了引擎资源却根本没进域切换。
                //
                // 这里不回落 per-set 共享布局兜底 —— 兜底的代价是静默:错配要等到录制期
                // 才以 VUID-00358 的形式暴露,那时已经离现场很远。
                //
                // 树外管线不受影响:它们不引用引擎契约资源(反射里没有 engine_set 条目
                // → owned 恒为空),走下面的 FeatureExplicit / PipelinePrivate 路径。
                return renderFailure<err::pipeline::ContractResourceNotMerged>(
                    rdesc::logicalResourceIndex(owned->name),
                    s
                );
            }

            // FEATURE/PASS domain: built from reflection (type/count) union
            // contract (binding_flags + stage). Stage source: for an entry
            // that hits the contract, THE CONTRACT'S STAGES ARE AUTHORITATIVE
            // — when the same logical resource (e.g. Instance's double
            // buffer) is referenced by different stage subsets across
            // multiple pipelines, its set instance is allocated by the
            // resource owner using the layout for the full stage union; a
            // per-pipeline reflected stage subset would produce a "not
            // identically defined" layout, making the bind at record time
            // incompatible. Only unregistered names (purely pass-private)
            // fall back to the reflected stage.
            const auto contractStages = [](uint8_t s) -> VkShaderStageFlags {
                VkShaderStageFlags f = 0;
                if (s & static_cast<uint8_t>(rdesc::EStageBits::VERTEX))
                    f |= VK_SHADER_STAGE_VERTEX_BIT;
                if (s & static_cast<uint8_t>(rdesc::EStageBits::FRAGMENT))
                    f |= VK_SHADER_STAGE_FRAGMENT_BIT;
                if (s & static_cast<uint8_t>(rdesc::EStageBits::COMPUTE))
                    f |= VK_SHADER_STAGE_COMPUTE_BIT;
                return f;
            };

            lux::cxx::SmallVector<VkDescriptorSetLayoutBinding, 8> bindings;
            lux::cxx::SmallVector<VkDescriptorBindingFlags, 8> flags;
            bool any_flags = false;
            bool update_after_bind = false;
            for (const auto& b : rset->bindings)
            {
                const auto* contract_entry = rdesc::findLogicalResource(b.name);
                const VkShaderStageFlags stage_flags =
                    contract_entry ? contractStages(contract_entry->stages) : b.stages;
                bindings.push_back({b.binding, b.type, b.count, stage_flags, nullptr});
                VkDescriptorBindingFlags f = 0;
                if (const auto* e = contract_entry)
                {
                    const auto ef = e->binding_flags;
                    if (ef & static_cast<uint8_t>(rdesc::EBindingFlags::UPDATE_AFTER_BIND))
                    {
                        f |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                        update_after_bind = true;
                    }
                    if (ef & static_cast<uint8_t>(rdesc::EBindingFlags::PARTIALLY_BOUND))
                        f |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
                    if (ef & static_cast<uint8_t>(rdesc::EBindingFlags::VARIABLE_COUNT))
                        f |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
                }
                flags.push_back(f);
                any_flags = any_flags || (f != 0);
            }

            // SPIR-V reflection order is declaration/decoration order, not a
            // canonical descriptor-binding order.  Keep the Vk layout and its
            // parallel binding-flags array sorted by binding number.  Besides
            // making layout interning deterministic, this is important for
            // descriptor-state consumers which flatten a layout by binding.
            // Water exposed the non-monotonic 0,2,1 case when LinearDepth was
            // injected after record-context allocation.
            for (std::size_t i = 1u; i < bindings.size(); ++i)
            {
                std::size_t cursor = i;
                while (cursor > 0u && bindings[cursor - 1u].binding > bindings[cursor].binding)
                {
                    std::swap(bindings[cursor - 1u], bindings[cursor]);
                    std::swap(flags[cursor - 1u], flags[cursor]);
                    --cursor;
                }
            }

            DescriptorLayoutDesc desc{
                .bindings = {bindings.data(), bindings.size()},
                .debug_name = debug_name + ".set" + std::to_string(s),
            };
            if (any_flags)
                desc.binding_flags = {flags.data(), flags.size()};
            if (update_after_bind)
                desc.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

            VkDescriptorSetLayout private_layout =
                descriptor_service_->layout(descriptor_service_->registerLayout(desc));
            out_set_layouts.push_back(private_layout);
            noteSlot(s, ESlotSource::PipelinePrivate, s, private_layout);
        }

        std::vector<VkPushConstantRange> pc(
            reflected.push_constant_ranges.begin(),
            reflected.push_constant_ranges.end()
        );
        return pipeline_layout_service_->getOrCreate({
            .set_layouts = {out_set_layouts.data(), out_set_layouts.size()},
            .push_constants = pc,
            .debug_name = debug_name,
        }
        );
    }

    Expected<GraphicsPipelineHandle> PipelineManager::registerGraphicsTemplate(
        const GraphicsPipelineTemplate& description,
        std::span<const rdesc::ShaderInfo* const> shader_infos
    )
    {
        const GraphicsPipelineHandle handle{static_cast<uint32_t>(pipeline_templates_.size())};

        GraphicsPipelineTemplate stored_template = description;
        lux::cxx::SmallVector<VkDescriptorSetLayout, 8> built_set_layouts;
        // The reflection data is kept alongside the template (LayoutAllocator
        // looks it back up at compile time, per pass -> template handle, to
        // aggregate the whole graph's resource requirements).
        std::optional<PipelineReflectedInfo> kept_reflection;

        // 1. Merge Shader information
        if (!shader_infos.empty())
        {
            PipelineReflectedInfo reflected = mergeShaderInfos(shader_infos);

            // If no layout is supplied, build one from reflection + contract
            // (see the file header). PC ranges: the caller's authoritative
            // ranges take priority (reflection can drop the VERTEX stage for
            // a fullscreen vertex shader with no PC block; description.push_constant_ranges
            // is the correction channel for that).
            if (stored_template.pipeline_layout == VK_NULL_HANDLE)
            {
                if (!description.push_constant_ranges.empty())
                {
                    reflected.push_constant_ranges.clear();
                    for (const auto& r : description.push_constant_ranges)
                        reflected.push_constant_ranges.push_back(r);
                }
                auto layout = buildReflectedPipelineLayout(reflected, description, built_set_layouts);
                if (!layout)
                    return lux::cxx::unexpected(layout.error());
                stored_template.pipeline_layout = *layout;
            }

            // 2. Save mask to template for RenderGraphCompiler use.
            //    If the caller explicitly set a non-zero mask (e.g. custom
            //    pipeline layouts), respect it instead of overwriting with
            //    the reflected mask.  A zero mask means "auto-detect".
            if (description.active_sets_mask == 0)
            {
                stored_template.active_sets_mask = reflected.active_sets_mask;
            }

            // resource_slot_map: "which engine set lives in this slot". If
            // the caller doesn't pre-fill it, it's synthesized here.
            //
            // SYNTHESIZE PER-SLOT FROM THE ACTUAL CLASSIFICATION, NOT AS AN
            // IDENTITY MAPPING OVER active_sets_mask. The old approach filled
            // {EDescriptorSetSlot(i), i} for every bit set in the mask, which
            // amounts to asserting "the slot number IS the engine-set
            // semantics" — and pipelines that borrow a slot violate exactly
            // that: the mesh pipelines put the GPU-culled visible-instance
            // table at set 5, and an identity synthesis would mislabel it as
            // Particle. ReflectedSlot has already recorded each slot's real
            // source and logical identity, so synthesizing from that is
            // correct: only engine sets and reflection holes carry engine
            // semantics; private / feature-explicit sets do not.
            //
            // (Right now this table only feeds two consumers: the fallback
            //  hint for reflection holes, and debug printing — descriptor
            //  binding at record time goes through the pass's explicitly
            //  declared ds_bindings and doesn't read this. But once the
            //  graph-compile-time Plan starts depending on this identity
            //  information, it needs to already be accurate.)
            if (stored_template.resource_slot_map.empty())
            {
                for (const auto& slot : reflected.slots)
                {
                    // Merged domain slot: one slot houses several canonical
                    // sets; member identity is looked up from the reflected
                    // name via the contract, and each is mapped individually
                    // to this same slot — at record time, bindImmutableDS
                    // calls for Instance/Light/Material/... all resolve here,
                    // and the binding plan then collapses them into a single
                    // domain-set bind.
                    if (slot.source == ESlotSource::DomainMerged)
                    {
                        const ReflectedSet* rs = nullptr;
                        for (const auto& cand : reflected.reflected_sets)
                            if (cand.set == slot.slot)
                            {
                                rs = &cand;
                                break;
                            }
                        if (!rs)
                            continue;
                        for (const auto& b : rs->bindings)
                        {
                            const auto* e = engineOwnedResource(b.name);
                            if (!e || e->canonical_set >= kDescriptorSetCount)
                                continue;
                            const auto logical = static_cast<EDescriptorSetSlot>(e->canonical_set);
                            bool seen = false;
                            for (const auto& [ls, li] : stored_template.resource_slot_map)
                                if (ls == logical && li == slot.slot)
                                {
                                    seen = true;
                                    break;
                                }
                            if (!seen)
                                stored_template.resource_slot_map.push_back({logical, slot.slot});
                        }
                        continue;
                    }
                    if (slot.source != ESlotSource::EngineShared && slot.source != ESlotSource::ReflectionHole)
                        continue;
                    if (slot.logical_set >= kDescriptorSetCount)
                        continue;
                    stored_template.resource_slot_map.push_back(
                        {static_cast<EDescriptorSetSlot>(slot.logical_set), slot.slot}
                    );
                }
            }

            // Save reflected PC ranges so vkCmdPushConstants callers can query
            // the correct stageFlags at record time (avoids VUID-01795/01796).
            // If the caller already provided authoritative ranges (e.g. from the
            // pipeline layout creation), keep them — reflection may under-report
            // stageFlags for fullscreen passes whose vertex shader has no PC block.
            if (description.push_constant_ranges.empty())
                stored_template.push_constant_ranges = reflected.push_constant_ranges;

            kept_reflection = std::move(reflected);
        }

        // Both paths must converge on a valid layout by this point (legacy
        // path: supplied by the caller; new path: built from reflection).
        if (stored_template.pipeline_layout == VK_NULL_HANDLE)
            return renderFailure<err::internal::InvalidArgument>();

        pipeline_templates_.push_back(stored_template);
        template_set_layouts_.push_back(std::move(built_set_layouts));
        template_reflections_.push_back(std::move(kept_reflection));
        template_layout_epochs_.push_back(0u);
        template_variant_masks_.push_back({});
        template_variant_fallback_counts_.push_back(0);
        return handle;
    }

    const GraphicsPipelineTemplate& PipelineManager::getTemplate(GraphicsPipelineHandle handle) const
    {
        assert(handle.index < pipeline_templates_.size());
        return pipeline_templates_[handle.index];
    }

    // (findTemplateForGeometry 已删:零调用点,且"按几何类型线性扫模板取第一个匹配"
    //  这个语义本身已被 variant bucket / family 管线取代。)

    VkRenderPass PipelineManager::getOrCreateRenderPass(const RenderPassKey& key)
    {
        auto it = render_pass_cache_.find(key);
        if (it != render_pass_cache_.end())
        {
            ++telemetry_.render_pass_cache_hits;
            return it->second;
        }

        ++telemetry_.render_pass_cache_misses;

        auto render_pass = create_render_pass_internal(key);
        if (!render_pass)
            return VK_NULL_HANDLE;
        render_pass_cache_.emplace(key, *render_pass);
        return *render_pass;
    }

    VkPipeline PipelineManager::getOrCreatePipeline(
        GraphicsPipelineHandle template_handle,
        const RenderPassKey& render_pass_key,
        uint32_t subpass_index
    )
    {
        return getOrCreatePipeline(template_handle, render_pass_key, subpass_index, 0);
    }

    VkPipeline PipelineManager::getOrCreatePipeline(
        GraphicsPipelineHandle template_handle,
        const RenderPassKey& render_pass_key,
        uint32_t subpass_index,
        ShaderFeatureMask features
    )
    {
        assert(template_handle.index < pipeline_templates_.size());
        const GraphicsPipelineTemplate& tmpl = pipeline_templates_[template_handle.index];

        auto& variant_masks = template_variant_masks_[template_handle.index];

        const auto ensureTrackedMask = [&](ShaderFeatureMask mask) {
            if (std::find(variant_masks.begin(), variant_masks.end(), mask) == variant_masks.end())
                variant_masks.push_back(mask);
        };

        // 预算满 → 退回无特性变体。这是图编译期的资源仲裁,调用方拿到的仍是一条
        // 可用管线,只是少了特性位;没有谁能就地处置,但画面会与请求的不一致,
        // 所以必须可见。上报通道按同键合并计数,原先那份手写幂次限流不必再有。
        if (features != 0)
        {
            const bool seen = std::find(variant_masks.begin(), variant_masks.end(), features) != variant_masks.end();
            if (!seen && variant_masks.size() >= kVariantBudgetPerTemplate)
            {
                if (error_sink_ != nullptr)
                    error_sink_->emit(
                        renderError<err::pipeline::VariantBudgetExhausted>(
                            template_handle.index,
                            kVariantBudgetPerTemplate
                        ),
                        RenderErrorEvent::kNoScene,
                        0
                    );
                features = 0;
            }
        }
        ensureTrackedMask(features);

        std::size_t specialization_hash_seed = 0;
        for (const auto& spec : tmpl.specialization_values)
        {
            hash_combine(specialization_hash_seed, static_cast<uint32_t>(spec.stage));
            hash_combine(specialization_hash_seed, spec.constant_id);
            hash_combine(specialization_hash_seed, spec.value);
        }

        PipelineKey key{};
        key.template_handle = template_handle;
        key.render_pass_key = render_pass_key;
        key.subpass_index = use_dynamic_rendering_ ? 0 : subpass_index;
        key.features = features;
        key.specialization_hash = static_cast<uint32_t>(specialization_hash_seed);
        // The layout version is part of the key — once the graph compiler
        // rewrites a template's layout, a PSO built against the old layout
        // must no longer be hit (finalizeTemplateLayout increments the
        // epoch).
        key.layout_epoch = templateLayoutEpoch(template_handle);

        auto it = pipeline_cache_.find(key);
        if (it != pipeline_cache_.end())
        {
            ++telemetry_.graphics_cache_hits;
            return it->second.pipeline;
        }
        ++telemetry_.graphics_cache_misses;
        // In dynamic rendering mode, no VkRenderPass is needed for pipeline creation.
        VkRenderPass render_pass = VK_NULL_HANDLE;
        if (!use_dynamic_rendering_)
        {
            render_pass = getOrCreateRenderPass(render_pass_key);
        }

        const auto create_started = std::chrono::steady_clock::now();
        VkPipeline pipeline = create_pipeline_internal(tmpl, render_pass, key.subpass_index, render_pass_key, features)
                                  .value_or(VK_NULL_HANDLE);
        const auto create_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_started)
                .count()
        );
        telemetry_.graphics_create_nanoseconds += create_nanoseconds;
        telemetry_.graphics_create_max_nanoseconds =
            std::max(telemetry_.graphics_create_max_nanoseconds, create_nanoseconds);

        if (pipeline == VK_NULL_HANDLE)
        {
            ++telemetry_.graphics_create_failures;
            // Do NOT cache a failed creation. An unconditional emplace would make
            // every future call with this key hit the cache and return
            // VK_NULL_HANDLE with no retry and no diagnostic, permanently disabling
            // this template/render-pass/feature combination after a *transient*
            // failure (driver OOM, a bad shader module during a hot-reload window).
            // Callers already treat VK_NULL_HANDLE as 'not ready', so leaving the
            // slot empty lets the next graph compile retry — matching the explicit
            // compute-path contract in registerComputePipeline.
            if (error_sink_ != nullptr)
                error_sink_->emit(
                    renderError<err::pipeline::GraphicsCreationFailed>(template_handle.index, features),
                    RenderErrorEvent::kNoScene,
                    0
                );
            return VK_NULL_HANDLE;
        }

        PipelineRecord record{};
        record.pipeline = pipeline;
        record.render_pass = render_pass;
        record.template_handle = template_handle;
        record.render_pass_key = render_pass_key;
        record.subpass_index = key.subpass_index;

        pipeline_cache_.emplace(key, record);

        return pipeline;
    }

    // (variantBudgetStats + VariantBudgetStats 已删:零调用点。两个统计成员
    //  template_variant_masks_ / template_variant_fallback_counts_ 保留 —— 变体
    //  预算超限的告警路径仍在读它们(见本文件 fallback_count 的幂次告警)。)

    Expected<VkRenderPass> PipelineManager::create_render_pass_internal(const RenderPassKey& key)
    {
        // Build attachment descriptions
        std::vector<VkAttachmentDescription> attachments;
        attachments.reserve(key.color_count + (key.depth_stencil_format == VkFormat::VK_FORMAT_UNDEFINED ? 0u : 1u));

        const VkSampleCountFlagBits samples = to_vk_sample_count(key.samples);

        // Color attachments
        for (uint32_t ci = 0; ci < key.color_count; ++ci)
        {
            VkAttachmentDescription desc{};
            desc.format = key.color_formats[ci];
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // Temporarily all LOAD, can be optimized later based on lifecycle
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments.push_back(desc);
        }

        // Depth attachment (if any)
        bool has_depth = (key.depth_stencil_format != VK_FORMAT_UNDEFINED);
        uint32_t depth_attachment_index = UINT32_MAX;

        if (has_depth)
        {
            depth_attachment_index = static_cast<uint32_t>(attachments.size());

            VkAttachmentDescription desc{};
            desc.format = key.depth_stencil_format;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // Similarly, can be changed to CLEAR later based on requirements
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            attachments.push_back(desc);
        }

        // Subpass description
        std::vector<VkAttachmentReference> color_refs;
        color_refs.reserve(key.color_count);

        for (uint32_t i = 0; i < key.color_count; ++i)
        {
            VkAttachmentReference ref{};
            ref.attachment = i;
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_refs.push_back(ref);
        }

        VkAttachmentReference depth_ref{};
        if (has_depth)
        {
            depth_ref.attachment = depth_attachment_index;
            depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkSubpassDescription subpass_template{};
        subpass_template.flags = 0;
        subpass_template.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass_template.inputAttachmentCount = 0;
        subpass_template.pInputAttachments = nullptr;
        subpass_template.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
        subpass_template.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
        subpass_template.pResolveAttachments = nullptr;
        subpass_template.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
        subpass_template.preserveAttachmentCount = 0;
        subpass_template.pPreserveAttachments = nullptr;

        std::vector<VkSubpassDescription> subpasses(key.subpass_count, subpass_template);

        // [FIX] Add simple dependencies between subpasses to match RGVulkanRecorder
        std::vector<VkSubpassDependency> dependencies;
        if (subpasses.size() > 1)
        {
            for (uint32_t k = 0; k < static_cast<uint32_t>(subpasses.size()) - 1; ++k)
            {
                VkSubpassDependency dep{};
                dep.srcSubpass = k;
                dep.dstSubpass = k + 1;
                // Conservative synchronization
                dep.srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                dep.dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                dep.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
                dep.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
                dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
                dependencies.push_back(dep);
            }
        }

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.empty() ? nullptr : attachments.data();
        info.subpassCount = static_cast<uint32_t>(subpasses.size());
        info.pSubpasses = subpasses.data();
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.empty() ? nullptr : dependencies.data();

        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkResult res = vkCreateRenderPass(device_ctx_->logicalDevice(), &info, nullptr, &render_pass);
        if (res != VK_SUCCESS)
            return renderFailure<err::device::RenderPassCreationFailed>(encodeVkResult(res));

        return render_pass;
    }

    Expected<VkPipeline> PipelineManager::create_pipeline_internal(
        const GraphicsPipelineTemplate& tmpl,
        VkRenderPass render_pass,
        uint32_t subpass_index,
        const RenderPassKey& render_pass_key,
        ShaderFeatureMask features
    )
    {
        assert(tmpl.pipeline_layout != VK_NULL_HANDLE);
        assert(tmpl.vertex_shader != VK_NULL_HANDLE);
        assert(tmpl.fragment_shader != VK_NULL_HANDLE);

        // 1) Shader stages
        VkPipelineShaderStageCreateInfo vert_stage{};
        vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = tmpl.vertex_shader;
        vert_stage.pName = tmpl.vertex_entry.c_str();

        VkPipelineShaderStageCreateInfo frag_stage{};
        frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = tmpl.fragment_shader;
        frag_stage.pName = tmpl.fragment_entry.c_str();

        struct StageSpecializationData
        {
            std::vector<VkSpecializationMapEntry> entries;
            std::vector<uint32_t> values;
            VkSpecializationInfo info{};

            void finalize()
            {
                for (uint32_t i = 0; i < static_cast<uint32_t>(entries.size()); ++i)
                    entries[i].offset = i * sizeof(uint32_t);
                info.mapEntryCount = static_cast<uint32_t>(entries.size());
                info.pMapEntries = entries.data();
                info.dataSize = values.size() * sizeof(uint32_t);
                info.pData = values.data();
            }

            void setValue(uint32_t constant_id, uint32_t value)
            {
                for (uint32_t i = 0; i < static_cast<uint32_t>(entries.size()); ++i)
                {
                    if (entries[i].constantID == constant_id)
                    {
                        values[i] = value;
                        return;
                    }
                }

                VkSpecializationMapEntry entry{};
                entry.constantID = constant_id;
                entry.size = sizeof(uint32_t);
                entries.push_back(entry);
                values.push_back(value);
            }
        };

        StageSpecializationData vert_spec{};
        StageSpecializationData frag_spec{};

        // Build specialization data for shader permutation features.
        if (features != 0)
        {
            const auto feature_spec = ShaderPermutationCompiler::buildSpecializationData(features);
            for (uint32_t i = 0; i < static_cast<uint32_t>(feature_spec.entries.size()); ++i)
            {
                const uint32_t constant_id = feature_spec.entries[i].constantID;
                const uint32_t value = feature_spec.values[i];
                vert_spec.setValue(constant_id, value);
                frag_spec.setValue(constant_id, value);
            }
        }

        // Template-level specialization overrides (e.g. MATERIAL_FAMILY variants).
        for (const auto& spec : tmpl.specialization_values)
        {
            if (spec.stage == VK_SHADER_STAGE_VERTEX_BIT)
                vert_spec.setValue(spec.constant_id, spec.value);
            else if (spec.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                frag_spec.setValue(spec.constant_id, spec.value);
        }

        if (!vert_spec.entries.empty())
        {
            vert_spec.finalize();
            vert_stage.pSpecializationInfo = &vert_spec.info;
        }
        if (!frag_spec.entries.empty())
        {
            frag_spec.finalize();
            frag_stage.pSpecializationInfo = &frag_spec.info;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {vert_stage, frag_stage};

        // 2) Vertex input
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(tmpl.vertex_bindings.size());
        vertex_input.pVertexBindingDescriptions = tmpl.vertex_bindings.empty() ? nullptr : tmpl.vertex_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(tmpl.vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions =
            tmpl.vertex_attributes.empty() ? nullptr : tmpl.vertex_attributes.data();

        // 3) Input assembly
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = tmpl.topology;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // 4) Viewport and scissor (usually using dynamic state)
        VkViewport dummy_viewport{};
        dummy_viewport.x = 0.0f;
        dummy_viewport.y = 0.0f;
        dummy_viewport.width = 1.0f;
        dummy_viewport.height = 1.0f;
        dummy_viewport.minDepth = 0.0f;
        dummy_viewport.maxDepth = 1.0f;

        VkRect2D dummy_scissor{};
        dummy_scissor.offset = {0, 0};
        dummy_scissor.extent = {1, 1};

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &dummy_viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &dummy_scissor;

        // 5) Rasterization
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.depthClampEnable = VK_FALSE;
        raster.rasterizerDiscardEnable = VK_FALSE;
        raster.polygonMode = tmpl.polygon_mode;
        raster.cullMode = tmpl.cull_mode;
        raster.frontFace = tmpl.front_face;
        raster.depthBiasEnable = tmpl.depth_bias_enable;
        raster.depthBiasConstantFactor = tmpl.depth_bias_constant;
        raster.depthBiasClamp = 0.0f;
        raster.depthBiasSlopeFactor = tmpl.depth_bias_slope;
        raster.lineWidth = tmpl.line_width;

        // 6) Multisample
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = to_vk_sample_count(render_pass_key.samples);
        multisample.sampleShadingEnable = VK_FALSE;
        multisample.minSampleShading = 1.0f;
        multisample.pSampleMask = nullptr;
        multisample.alphaToCoverageEnable = VK_FALSE;
        multisample.alphaToOneEnable = VK_FALSE;

        // 7) Depth/Stencil
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = tmpl.depth_test_enable;
        depth_stencil.depthWriteEnable = tmpl.depth_write_enable;
        depth_stencil.depthCompareOp = tmpl.depth_compare_op;
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;
        depth_stencil.minDepthBounds = 0.0f;
        depth_stencil.maxDepthBounds = 1.0f;

        // 8) Color blending
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
        blend_attachments.resize(render_pass_key.color_count);

        for (auto& a : blend_attachments)
        {
            a.blendEnable = tmpl.blend_enable;
            a.colorWriteMask = tmpl.color_write_mask;
            if (tmpl.blend_enable)
            {
                a.srcColorBlendFactor = tmpl.src_color_blend_factor;
                a.dstColorBlendFactor = tmpl.dst_color_blend_factor;
                a.colorBlendOp = tmpl.color_blend_op;
                a.srcAlphaBlendFactor = tmpl.src_alpha_blend_factor;
                a.dstAlphaBlendFactor = tmpl.dst_alpha_blend_factor;
                a.alphaBlendOp = tmpl.alpha_blend_op;
            }
            else
            {
                a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                a.colorBlendOp = VK_BLEND_OP_ADD;
                a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                a.alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.logicOpEnable = VK_FALSE;
        color_blend.logicOp = VK_LOGIC_OP_COPY;
        color_blend.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
        color_blend.pAttachments = blend_attachments.empty() ? nullptr : blend_attachments.data();
        color_blend.blendConstants[0] = 0.0f;
        color_blend.blendConstants[1] = 0.0f;
        color_blend.blendConstants[2] = 0.0f;
        color_blend.blendConstants[3] = 0.0f;

        // 9) Dynamic state
        std::vector<VkDynamicState> dynamic_states;
        if (tmpl.use_dynamic_viewport)
            dynamic_states.push_back(VK_DYNAMIC_STATE_VIEWPORT);
        if (tmpl.use_dynamic_scissor)
            dynamic_states.push_back(VK_DYNAMIC_STATE_SCISSOR);
        if (tmpl.depth_bias_enable)
            dynamic_states.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);

        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.empty() ? nullptr : dynamic_states.data();

        // 10) Total pipeline creation info
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth_stencil;
        info.pColorBlendState = &color_blend;
        info.pDynamicState = dynamic_states.empty() ? nullptr : &dynamic_state;
        info.layout = tmpl.pipeline_layout;
        info.basePipelineHandle = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;

        // Dynamic rendering (Vulkan 1.3): use VkPipelineRenderingCreateInfo instead of VkRenderPass
        VkPipelineRenderingCreateInfo rendering_info{};
        // Local-read merged-scope remaps — must outlive the
        // vkCreateGraphicsPipelines call, hence declared at this scope.
        VkRenderingAttachmentLocationInfo lr_loc_info{};
        VkRenderingInputAttachmentIndexInfo lr_input_info{};
        if (use_dynamic_rendering_)
        {
            rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering_info.pNext = nullptr;
            rendering_info.viewMask = 0;
            rendering_info.colorAttachmentCount = render_pass_key.color_count;
            rendering_info.pColorAttachmentFormats = render_pass_key.color_formats.data();
            rendering_info.depthAttachmentFormat = render_pass_key.depth_stencil_format;
            rendering_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

            // The remap arrays are sized for the local-read merged scope. When
            // the pipeline is built against a NON-merged pass key (e.g. the
            // lighting feature fell back to SAMPLED and the g-buffer group kept
            // its own 3 colors), the counts differ — skip the chain entirely;
            // the spec default is the identity mapping, which is correct there.
            if (!tmpl.lr_color_locations.empty() && tmpl.lr_color_locations.size() == render_pass_key.color_count)
            {
                lr_loc_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO;
                lr_loc_info.colorAttachmentCount = static_cast<uint32_t>(tmpl.lr_color_locations.size());
                lr_loc_info.pColorAttachmentLocations = tmpl.lr_color_locations.data();
                lr_loc_info.pNext = rendering_info.pNext;
                rendering_info.pNext = &lr_loc_info;
            }
            if (!tmpl.lr_input_indices.empty() && tmpl.lr_input_indices.size() == render_pass_key.color_count)
            {
                lr_input_info.sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO;
                lr_input_info.colorAttachmentCount = static_cast<uint32_t>(tmpl.lr_input_indices.size());
                lr_input_info.pColorAttachmentInputIndices = tmpl.lr_input_indices.data();
                lr_input_info.pDepthInputAttachmentIndex =
                    (tmpl.lr_depth_input_index != VK_ATTACHMENT_UNUSED) ? &tmpl.lr_depth_input_index : nullptr;
                lr_input_info.pNext = rendering_info.pNext;
                rendering_info.pNext = &lr_input_info;
            }

            info.pNext = &rendering_info;
            info.renderPass = VK_NULL_HANDLE;
            info.subpass = 0;
        }
        else
        {
            info.renderPass = render_pass;
            info.subpass = subpass_index;
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult res =
            vkCreateGraphicsPipelines(device_ctx_->logicalDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (res != VK_SUCCESS)
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(res));

        return pipeline;
    }

} // namespace lux::render
