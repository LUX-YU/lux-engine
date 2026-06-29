#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/ShaderPermutationCompiler.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <cassert>
#include <algorithm>
#include <map>
#include <iostream>

namespace lux::render
{
    // Helper: Convert lux::rdesc::EShaderType to VkShaderStageFlags
    static VkShaderStageFlags getVkShaderStage(rdesc::EShaderType type) {
        switch (type) {
            case rdesc::EShaderType::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
            case rdesc::EShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case rdesc::EShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case rdesc::EShaderType::COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
            case rdesc::EShaderType::TESSELLATION_CTRL: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case rdesc::EShaderType::TESSELLATION_EVAL: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            case rdesc::EShaderType::RAYGEN: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case rdesc::EShaderType::ANY_HIT: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case rdesc::EShaderType::CLOSEST_HIT: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case rdesc::EShaderType::MISS: return VK_SHADER_STAGE_MISS_BIT_KHR;
            case rdesc::EShaderType::INTERSECTION: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case rdesc::EShaderType::CALLABLE: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            default: return 0;
        }
    }

    static PipelineReflectedInfo mergeShaderInfos(const std::vector<const rdesc::ShaderInfo*>& shaders)
    {
        PipelineReflectedInfo result;
        
        // Temporary storage for Push Constants: offset -> range
        // Used to merge stageFlags for ranges with the same offset
        struct PCRange {
            uint32_t size;
            VkShaderStageFlags stages;
        };
        std::map<uint32_t, PCRange> pc_map;

        for (const auto* shader : shaders)
        {
            if (!shader) continue;

            // Deduce the Stage of the current Shader
            VkShaderStageFlags stage_flags = 0;
            if (!shader->entry_points.empty()) {
                stage_flags = getVkShaderStage(shader->entry_points[0].stage);
            }

            // 1. Merge Descriptor Sets (Calculate Mask)
            for (const auto& set_info : shader->sets) {
                // If this Set appears in any Shader, mark it as active
                result.active_sets_mask |= (1u << set_info.set);
            }

            // 2. Merge Push Constants
            for (const auto& pc : shader->push_constants) {
                auto it = pc_map.find(pc.offset);
                if (it == pc_map.end()) {
                    pc_map[pc.offset] = { pc.size, stage_flags };
                } else {
                    // Simple merge check: if offset is the same, size should be the same (or a union)
                    // And accumulate stageFlags
                    it->second.stages |= stage_flags;
                    it->second.size = std::max(it->second.size, pc.size);
                }
            }
        }

        // Convert Push Constants to Vulkan format
        for (const auto& [offset, range] : pc_map) {
            VkPushConstantRange vk_range{};
            vk_range.stageFlags = range.stages;
            vk_range.offset = offset;
            vk_range.size = range.size;
            result.push_constant_ranges.push_back(vk_range);
        }

        return result;
    }

    bool RenderPassKey::is_compatible_with(const RenderPassKey& other) const noexcept
    {
        if (samples != other.samples)
            return false;

        if (color_count != other.color_count)
            return false;

        for (uint32_t i = 0; i < color_count; ++i)
        {
            if (color_formats[i] != other.color_formats[i])
                return false;
        }

        if (depth_stencil_format == VkFormat::VK_FORMAT_UNDEFINED)
            return false;

        if (depth_stencil_format != other.depth_stencil_format)
        {
            return false;
        }

        return true;
    }

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

        pipeline_templates_.clear();
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

    ComputePipelineHandle PipelineManager::registerComputePipeline(
        VkShaderModule   shader,
        VkPipelineLayout layout,
        std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specialization_values)
    {
        const ComputePipelineHandle handle{ static_cast<uint32_t>(compute_pipelines_.size()) };

        std::vector<VkSpecializationMapEntry> spec_entries;
        std::vector<uint32_t>                 spec_values;
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
        ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader;
        ci.stage.pName  = "main";
        ci.stage.pSpecializationInfo = spec_entries.empty() ? nullptr : &spec_info;

        VkPipeline vk_pipeline = VK_NULL_HANDLE;
        const VkResult res = vkCreateComputePipelines(device_ctx_->logicalDevice(), VK_NULL_HANDLE,
                                                      1, &ci, nullptr, &vk_pipeline);
        if (res != VK_SUCCESS)
        {
            // Do not consume a pipeline slot on failure. Returning an
            // index-valid handle wrapping VK_NULL_HANDLE would make
            // ComputePipelineHandle::valid() report true and silently feed a
            // null pipeline into vkCmdBindPipeline/vkCmdDispatch. Return the
            // sentinel instead so callers' .valid() guards (and the RG
            // compile-time null check) detect the failure and can retry later.
            std::cerr << "[PipelineManager] vkCreateComputePipelines failed: "
                      << res << '\n';
            return kInvalidComputePipelineHandle;
        }

        compute_pipelines_.push_back({ vk_pipeline, layout });
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

    Expected<GraphicsPipelineHandle> PipelineManager::registerGraphicsTemplate(
        const GraphicsPipelineTemplate& description,
        const std::vector<const rdesc::ShaderInfo*>& shader_infos)
    {
        const GraphicsPipelineHandle handle{
            static_cast<uint32_t>(pipeline_templates_.size())};

        GraphicsPipelineTemplate stored_template = description;

        if (stored_template.pipeline_layout == VK_NULL_HANDLE)
            return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));

        // 1. Merge Shader information
        if (!shader_infos.empty()) {
            PipelineReflectedInfo reflected = mergeShaderInfos(shader_infos);
            
            // 2. Save mask to template for RenderGraphCompiler use.
            //    If the caller explicitly set a non-zero mask (e.g. custom
            //    pipeline layouts), respect it instead of overwriting with
            //    the reflected mask.  A zero mask means "auto-detect".
            if (description.active_sets_mask == 0) {
                stored_template.active_sets_mask = reflected.active_sets_mask;
            }

            // Populate resource_slot_map with the identity mapping (named slot → same index)
            // for every active set, unless the caller pre-filled it (non-standard layout).
            if (stored_template.resource_slot_map.empty()) {
                const uint32_t effective_mask = stored_template.active_sets_mask;
                for (uint32_t i = 0; i < kDescriptorSetCount; ++i) {
                    if (effective_mask & (1u << i)) {
                        stored_template.resource_slot_map.push_back(
                            {static_cast<EDescriptorSetSlot>(i), i});
                    }
                }
            }

            // Save reflected PC ranges so vkCmdPushConstants callers can query
            // the correct stageFlags at record time (avoids VUID-01795/01796).
            // If the caller already provided authoritative ranges (e.g. from the
            // pipeline layout creation), keep them — reflection may under-report
            // stageFlags for fullscreen passes whose vertex shader has no PC block.
            if (description.push_constant_ranges.empty())
                stored_template.push_constant_ranges = reflected.push_constant_ranges;

        }

        pipeline_templates_.push_back(stored_template);
        template_variant_masks_.push_back({});
        template_variant_fallback_counts_.push_back(0);
        return handle;
    }

    const GraphicsPipelineTemplate& PipelineManager::getTemplate(GraphicsPipelineHandle handle) const
    {
        assert(handle.index < pipeline_templates_.size());
        return pipeline_templates_[handle.index];
    }

    GraphicsPipelineHandle PipelineManager::findTemplateForGeometry(EGeometryType type) const noexcept
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(pipeline_templates_.size()); ++i)
        {
            if (pipeline_templates_[i].geometry_type == type)
                return GraphicsPipelineHandle{i};
        }
        return kInvalidPipelineHandle;
    }

    VkRenderPass PipelineManager::getOrCreateRenderPass(const RenderPassKey& key)
    {
        auto it = render_pass_cache_.find(key);
        if (it != render_pass_cache_.end())
            return it->second;

        auto render_pass = create_render_pass_internal(key);
        if (!render_pass)
            return VK_NULL_HANDLE;
        render_pass_cache_.emplace(key, *render_pass);
        return *render_pass;
    }

    VkPipeline PipelineManager::getOrCreatePipeline(GraphicsPipelineHandle template_handle, const RenderPassKey& render_pass_key, uint32_t subpass_index)
    {
        return getOrCreatePipeline(template_handle, render_pass_key, subpass_index, 0);
    }

    VkPipeline PipelineManager::getOrCreatePipeline(GraphicsPipelineHandle template_handle, const RenderPassKey& render_pass_key, uint32_t subpass_index, ShaderFeatureMask features)
    {
        assert(template_handle.index < pipeline_templates_.size());
        const GraphicsPipelineTemplate& tmpl = pipeline_templates_[template_handle.index];

        auto& variant_masks = template_variant_masks_[template_handle.index];
        auto& fallback_count = template_variant_fallback_counts_[template_handle.index];

        const auto ensureTrackedMask = [&](ShaderFeatureMask mask)
        {
            if (std::find(variant_masks.begin(), variant_masks.end(), mask) == variant_masks.end())
                variant_masks.push_back(mask);
        };

        if (features != 0)
        {
            const bool seen = std::find(variant_masks.begin(), variant_masks.end(), features) != variant_masks.end();
            if (!seen && variant_masks.size() >= kVariantBudgetPerTemplate)
            {
                ++fallback_count;
                if ((fallback_count & (fallback_count - 1)) == 0)
                {
                    std::cerr << "[PipelineManager] variant budget reached for template "
                              << template_handle.index
                              << " (budget=" << kVariantBudgetPerTemplate
                              << ", fallback_count=" << fallback_count
                              << ")\n";
                }
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
        key.subpass_index   = use_dynamic_rendering_ ? 0 : subpass_index;
        key.features        = features;
        key.specialization_hash = static_cast<uint32_t>(specialization_hash_seed);

        auto it = pipeline_cache_.find(key);
        if (it != pipeline_cache_.end())
        {
            return it->second.pipeline;
        }
        // In dynamic rendering mode, no VkRenderPass is needed for pipeline creation.
        VkRenderPass render_pass = VK_NULL_HANDLE;
        if (!use_dynamic_rendering_)
        {
            render_pass = getOrCreateRenderPass(render_pass_key);
        }

        VkPipeline pipeline = create_pipeline_internal(tmpl, render_pass, key.subpass_index, render_pass_key, features).value_or(VK_NULL_HANDLE);

        if (pipeline == VK_NULL_HANDLE)
        {
            // Do NOT cache a failed creation. An unconditional emplace would make
            // every future call with this key hit the cache and return
            // VK_NULL_HANDLE with no retry and no diagnostic, permanently disabling
            // this template/render-pass/feature combination after a *transient*
            // failure (driver OOM, a bad shader module during a hot-reload window).
            // Callers already treat VK_NULL_HANDLE as 'not ready', so leaving the
            // slot empty lets the next graph compile retry — matching the explicit
            // compute-path contract in registerComputePipeline. (medium)
            std::cerr << "[PipelineManager] graphics pipeline creation failed (template="
                      << template_handle.index << ", features=" << features
                      << "); not caching, will retry on next compile\n";
            return VK_NULL_HANDLE;
        }

        PipelineRecord record{};
        record.pipeline        = pipeline;
        record.render_pass     = render_pass;
        record.template_handle = template_handle;
        record.render_pass_key = render_pass_key;
        record.subpass_index   = key.subpass_index;

        pipeline_cache_.emplace(key, record);

        return pipeline;
    }

    PipelineManager::VariantBudgetStats PipelineManager::variantBudgetStats(GraphicsPipelineHandle handle) const noexcept
    {
        if (!handle.valid() || handle.index >= template_variant_masks_.size())
            return {};
        return VariantBudgetStats{
            .unique_feature_masks = static_cast<uint32_t>(template_variant_masks_[handle.index].size()),
            .fallback_count = template_variant_fallback_counts_[handle.index],
        };
    }

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
            desc.format         = key.color_formats[ci];
            desc.samples        = samples;
            desc.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;   // Temporarily all LOAD, can be optimized later based on lifecycle
            desc.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            desc.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments.push_back(desc);
        }

        // Depth attachment (if any)
        bool has_depth = (key.depth_stencil_format != VK_FORMAT_UNDEFINED);
        uint32_t depth_attachment_index = UINT32_MAX;

        if (has_depth)
        {
            depth_attachment_index = static_cast<uint32_t>(attachments.size());

            VkAttachmentDescription desc{};
            desc.format         = key.depth_stencil_format;
            desc.samples        = samples;
            desc.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;   // Similarly, can be changed to CLEAR later based on requirements
            desc.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            desc.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            attachments.push_back(desc);
        }

        // Subpass description
        std::vector<VkAttachmentReference> color_refs;
        color_refs.reserve(key.color_count);

        for (uint32_t i = 0; i < key.color_count; ++i)
        {
            VkAttachmentReference ref{};
            ref.attachment = i;
            ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_refs.push_back(ref);
        }

        VkAttachmentReference depth_ref{};
        if (has_depth)
        {
            depth_ref.attachment = depth_attachment_index;
            depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkSubpassDescription subpass_template{};
        subpass_template.flags                   = 0;
        subpass_template.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass_template.inputAttachmentCount    = 0;
        subpass_template.pInputAttachments       = nullptr;
        subpass_template.colorAttachmentCount    = static_cast<uint32_t>(color_refs.size());
        subpass_template.pColorAttachments       = color_refs.empty() ? nullptr : color_refs.data();
        subpass_template.pResolveAttachments     = nullptr;
        subpass_template.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
        subpass_template.preserveAttachmentCount = 0;
        subpass_template.pPreserveAttachments    = nullptr;

        std::vector<VkSubpassDescription> subpasses(key.subpass_count, subpass_template);

        // [FIX] Add simple dependencies between subpasses to match RGVulkanRecorder
        std::vector<VkSubpassDependency> dependencies;
        if (subpasses.size() > 1) {
            for(uint32_t k=0; k < static_cast<uint32_t>(subpasses.size()) - 1; ++k) {
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
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments    = attachments.empty() ? nullptr : attachments.data();
        info.subpassCount    = static_cast<uint32_t>(subpasses.size());
        info.pSubpasses      = subpasses.data();
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies   = dependencies.empty() ? nullptr : dependencies.data();

        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkResult res = vkCreateRenderPass(device_ctx_->logicalDevice(), &info, nullptr, &render_pass);
        if (res != VK_SUCCESS)
            return lux::cxx::unexpected(make_error_code(ERenderError::RenderPassCreateFailed));

        return render_pass;
    }

    Expected<VkPipeline> PipelineManager::create_pipeline_internal(const GraphicsPipelineTemplate& tmpl, VkRenderPass render_pass, uint32_t subpass_index, const RenderPassKey& render_pass_key, ShaderFeatureMask features)
    {
        assert(tmpl.pipeline_layout != VK_NULL_HANDLE);
        assert(tmpl.vertex_shader   != VK_NULL_HANDLE);
        assert(tmpl.fragment_shader != VK_NULL_HANDLE);

        // 1) Shader stages
        VkPipelineShaderStageCreateInfo vert_stage{};
        vert_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = tmpl.vertex_shader;
        vert_stage.pName  = tmpl.vertex_entry.c_str();

        VkPipelineShaderStageCreateInfo frag_stage{};
        frag_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = tmpl.fragment_shader;
        frag_stage.pName  = tmpl.fragment_entry.c_str();

        struct StageSpecializationData
        {
            std::vector<VkSpecializationMapEntry> entries;
            std::vector<uint32_t>                 values;
            VkSpecializationInfo                  info{};

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

        VkPipelineShaderStageCreateInfo stages[2] = { vert_stage, frag_stage };

        // 2) Vertex input
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount   = static_cast<uint32_t>(tmpl.vertex_bindings.size());
        vertex_input.pVertexBindingDescriptions      = tmpl.vertex_bindings.empty() ? nullptr : tmpl.vertex_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(tmpl.vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions    = tmpl.vertex_attributes.empty() ? nullptr : tmpl.vertex_attributes.data();

        // 3) Input assembly
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology               = tmpl.topology;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // 4) Viewport and scissor (usually using dynamic state)
        VkViewport dummy_viewport{};
        dummy_viewport.x        = 0.0f;
        dummy_viewport.y        = 0.0f;
        dummy_viewport.width    = 1.0f;
        dummy_viewport.height   = 1.0f;
        dummy_viewport.minDepth = 0.0f;
        dummy_viewport.maxDepth = 1.0f;

        VkRect2D dummy_scissor{};
        dummy_scissor.offset = { 0, 0 };
        dummy_scissor.extent = { 1, 1 };

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.pViewports    = &dummy_viewport;
        viewport_state.scissorCount  = 1;
        viewport_state.pScissors     = &dummy_scissor;

        // 5) Rasterization
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.depthClampEnable        = VK_FALSE;
        raster.rasterizerDiscardEnable = VK_FALSE;
        raster.polygonMode             = tmpl.polygon_mode;
        raster.cullMode                = tmpl.cull_mode;
        raster.frontFace               = tmpl.front_face;
        raster.depthBiasEnable         = tmpl.depth_bias_enable;
        raster.depthBiasConstantFactor = tmpl.depth_bias_constant;
        raster.depthBiasClamp          = 0.0f;
        raster.depthBiasSlopeFactor    = tmpl.depth_bias_slope;
        raster.lineWidth               = tmpl.line_width;

        // 6) Multisample
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples  = to_vk_sample_count(render_pass_key.samples);
        multisample.sampleShadingEnable   = VK_FALSE;
        multisample.minSampleShading      = 1.0f;
        multisample.pSampleMask           = nullptr;
        multisample.alphaToCoverageEnable = VK_FALSE;
        multisample.alphaToOneEnable      = VK_FALSE;

        // 7) Depth/Stencil
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable       = tmpl.depth_test_enable;
        depth_stencil.depthWriteEnable      = tmpl.depth_write_enable;
        depth_stencil.depthCompareOp        = tmpl.depth_compare_op;
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable     = VK_FALSE;
        depth_stencil.minDepthBounds        = 0.0f;
        depth_stencil.maxDepthBounds        = 1.0f;

        // 8) Color blending
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
        blend_attachments.resize(render_pass_key.color_count);

        for (auto& a : blend_attachments)
        {
            a.blendEnable         = tmpl.blend_enable;
            a.colorWriteMask      = tmpl.color_write_mask;
            if (tmpl.blend_enable)
            {
                a.srcColorBlendFactor = tmpl.src_color_blend_factor;
                a.dstColorBlendFactor = tmpl.dst_color_blend_factor;
                a.colorBlendOp        = tmpl.color_blend_op;
                a.srcAlphaBlendFactor = tmpl.src_alpha_blend_factor;
                a.dstAlphaBlendFactor = tmpl.dst_alpha_blend_factor;
                a.alphaBlendOp        = tmpl.alpha_blend_op;
            }
            else
            {
                a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                a.colorBlendOp        = VK_BLEND_OP_ADD;
                a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                a.alphaBlendOp        = VK_BLEND_OP_ADD;
            }
        }

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.logicOpEnable   = VK_FALSE;
        color_blend.logicOp         = VK_LOGIC_OP_COPY;
        color_blend.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
        color_blend.pAttachments    = blend_attachments.empty() ? nullptr : blend_attachments.data();
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
        dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates    = dynamic_states.empty() ? nullptr : dynamic_states.data();

        // 10) Total pipeline creation info
        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState      = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth_stencil;
        info.pColorBlendState    = &color_blend;
        info.pDynamicState       = dynamic_states.empty() ? nullptr : &dynamic_state;
        info.layout              = tmpl.pipeline_layout;
        info.basePipelineHandle  = VK_NULL_HANDLE;
        info.basePipelineIndex   = -1;

        // Dynamic rendering (Vulkan 1.3): use VkPipelineRenderingCreateInfo instead of VkRenderPass
        VkPipelineRenderingCreateInfo rendering_info{};
        if (use_dynamic_rendering_)
        {
            rendering_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering_info.pNext                   = nullptr;
            rendering_info.viewMask                = 0;
            rendering_info.colorAttachmentCount    = render_pass_key.color_count;
            rendering_info.pColorAttachmentFormats = render_pass_key.color_formats.data();
            rendering_info.depthAttachmentFormat   = render_pass_key.depth_stencil_format;
            rendering_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

            info.pNext      = &rendering_info;
            info.renderPass = VK_NULL_HANDLE;
            info.subpass    = 0;
        }
        else
        {
            info.renderPass = render_pass;
            info.subpass    = subpass_index;
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult res = vkCreateGraphicsPipelines(
            device_ctx_->logicalDevice(),
            VK_NULL_HANDLE,
            1,
            &info,
            nullptr,
            &pipeline
        );
        if (res != VK_SUCCESS)
            return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

        return pipeline;
    }

} // namespace lux::render
