#include <algorithm>
#include <lux/engine/render/vulkan/shader/Format.hpp>
#include <lux/engine/render/vulkan/shader/Program.hpp>
#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>
#include <spirv-headers/spirv.hpp>

namespace lux::render::vulkan
{
    namespace
    {
        bool supportedInterface(const toolchain::PassStageInterface& interface, const VulkanDevice& device) noexcept
        {
            // Matches the features actually enabled by the existing Device factory, not merely physical support.
            const auto& limits = device.properties().limits;
            for (const auto capability : interface.capabilities)
            {
                switch (static_cast<spv::Capability>(capability))
                {
                case spv::CapabilityMatrix:
                case spv::CapabilityShader:
                case spv::CapabilityImageQuery:
                case spv::CapabilityDerivativeControl:
                    break;
                case spv::CapabilityMultiView:
                    if (!device.multiview())
                    {
                        return false;
                    }
                    break;
                case spv::CapabilityInputAttachment:
                    if (!device.localRead())
                    {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
            }
            if (interface.stage == 4)
            {
                if (interface.local_size_id)
                {
                    return false; // maintenance4 was not enabled.
                }
                std::uint64_t invocations = 1;
                for (unsigned i = 0; i < 3; ++i)
                {
                    const auto size = interface.workgroup[i];
                    if (size == 0 || size > limits.maxComputeWorkGroupSize[i] ||
                        invocations > limits.maxComputeWorkGroupInvocations / size)
                    {
                        return false;
                    }
                    invocations *= size;
                }
            }
            return true;
        }

        bool matchesScalar(VkFormat format, toolchain::EShaderScalarType scalar) noexcept
        {
            const auto category = rdesc::textureClearClass(neutralTextureFormat(format));
            switch (scalar)
            {
            case toolchain::EShaderScalarType::FLOAT:
                return category == rdesc::ETextureClearClass::FLOAT;
            case toolchain::EShaderScalarType::SINT:
                return category == rdesc::ETextureClearClass::SINT;
            case toolchain::EShaderScalarType::UINT:
                return category == rdesc::ETextureClearClass::UINT;
            }
            return false;
        }

        bool matchesInterfaces(
            const toolchain::ShaderVariantIdentity& variant,
            const GraphicsDescription& description,
            const VkPhysicalDeviceLimits& limits
        ) noexcept
        {
            for (const auto& stage : variant.stages)
            {
                auto interface = toolchain::reflectPassInterface(stage.words);
                if (!interface)
                {
                    return false;
                }
                const auto& locations = stage.stage == 1 ? interface->inputs : interface->outputs;
                for (const auto& field : locations)
                {
                    if (stage.stage == 1)
                    {
                        const auto attribute = std::find_if(
                            description.attributes.begin(),
                            description.attributes.end(),
                            [&](const auto& value) { return value.location == field.location; }
                        );
                        if (attribute == description.attributes.end() ||
                            !matchesScalar(attribute->format, field.scalar))
                        {
                            return false;
                        }
                    }
                    else if (field.location >= description.color_formats.size() ||
                             !matchesScalar(description.color_formats[field.location], field.scalar))
                    {
                        return false;
                    }
                }
                std::uint64_t total_components = 0;
                for (const auto& field : stage.stage == 1 ? interface->outputs : interface->inputs)
                {
                    total_components += field.components;
                }
                const auto limit =
                    stage.stage == 1 ? limits.maxVertexOutputComponents : limits.maxFragmentInputComponents;
                if (total_components > limit)
                {
                    return false;
                }
            }
            return true;
        }

        bool matchesAttachments(
            const LogicalGraphIdentity& graph,
            const LogicalPass& pass,
            const GraphicsDescription& description
        ) noexcept
        {
            std::vector<VkFormat> colors;
            VkFormat depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
            for (const auto& field : pass.bindings)
            {
                const auto* attachment = std::get_if<AttachmentBinding>(&field.value);
                if (!attachment || attachment->role == rdesc::EPassFieldRole::RESOLVE)
                {
                    continue;
                }
                const auto& texture = graph.resources[attachment->resource.value() - 1].texture();
                const auto format = nativeTextureFormat(texture.format);
                if (texture.samples != static_cast<std::uint32_t>(description.samples))
                {
                    return false;
                }
                if (attachment->role == rdesc::EPassFieldRole::COLOR_ATTACHMENT)
                {
                    colors.push_back(format);
                }
                else
                {
                    const auto aspects = static_cast<std::uint32_t>(attachment->range.aspect);
                    if ((aspects & static_cast<std::uint32_t>(EAspect::DEPTH)) != 0)
                    {
                        depth = format;
                    }
                    if ((aspects & static_cast<std::uint32_t>(EAspect::STENCIL)) != 0)
                    {
                        stencil = format;
                    }
                }
            }
            return colors == description.color_formats && depth == description.depth_format &&
                   stencil == description.stencil_format;
        }

        bool matchesContract(const LogicalPass& pass, const rdesc::PassShaderContract& schema) noexcept
        {
            const bool wrong_schema =
                pass.schema_name != schema.canonical_name || pass.shader_declarations != schema.declarations ||
                pass.scalar_fields.size() != schema.scalars.size() || pass.scalar_size != schema.parameter_size;
            if (wrong_schema)
            {
                return false;
            }
            for (std::size_t i = 0; i < schema.scalars.size(); ++i)
            {
                const auto& a = schema.scalars[i];
                const auto& b = pass.scalar_fields[i];
                const bool mismatch = a.path != b.path || a.kind != b.kind || a.offset != b.offset ||
                                      a.size != b.size || a.array_stride != b.array_stride ||
                                      a.array_count != b.array_count || a.owner != b.owner ||
                                      a.frequency != b.frequency || a.stages != b.stages;
                if (mismatch)
                {
                    return false;
                }
            }
            std::size_t binding_count = 0;
            for (const auto& field : schema.resources)
            {
                const auto count = field.array_count;
                for (std::uint32_t element = 0; element < count; ++element)
                {
                    const auto found = std::find_if(
                        pass.bindings.begin(),
                        pass.bindings.end(),
                        [&](const auto& b) { return b.path == field.path && b.array_element == element; }
                    );
                    if (found == pass.bindings.end())
                    {
                        return false;
                    }
                    const bool mismatch = found->shader_name != field.shader_name ||
                                          found->semantic != field.semantic || found->stages != field.stages ||
                                          found->owner != field.owner || found->frequency != field.frequency ||
                                          found->required != field.required;
                    if (mismatch)
                    {
                        return false;
                    }
                    const bool matches_role = std::visit(
                        [&](const auto& binding)
                        {
                            using T = std::decay_t<decltype(binding)>;
                            if constexpr (std::is_same_v<T, SamplerBinding>)
                            {
                                return field.role == rdesc::EPassFieldRole::SAMPLER &&
                                       binding.paired_texture == field.paired_texture;
                            }
                            else if constexpr (std::is_same_v<T, ShaderResourceBinding>)
                            {
                                return binding.role == field.role && binding.dimension == field.dimension &&
                                       binding.image_format == field.image_format &&
                                       binding.array_count == field.array_count &&
                                       binding.element_stride == field.element_stride &&
                                       binding.descriptor_array == field.descriptor_array;
                            }
                            else
                            {
                                return binding.role == field.role;
                            }
                        },
                        found->value
                    );
                    if (!matches_role)
                    {
                        return false;
                    }
                    ++binding_count;
                }
            }
            return binding_count == pass.bindings.size();
        }
    } // namespace

    RenderResult<NativeShaderProgram> NativeShaderProgram::create(
        const VulkanDevice& device,
        const LogicalGraphPlan& graph,
        GraphPassId pass,
        const toolchain::CompiledShaderVariant& shader,
        const rdesc::PassShaderContract& schema,
        const OwnerAssignment& assignment,
        std::span<const OwnerShape> owners,
        VPipelineDescription description
    ) noexcept
    {
        const bool invalid_pass = !pass.isValid() || pass.value() > graph.identity().passes.size();
        if (invalid_pass)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto index = pass.value() - 1;
        const auto& logical = graph.identity().passes[index];
        const auto& variant = shader.identity();
        const ShaderReference reference{variant.inputs.asset_name, variant.inputs.variant_name};
        if (!graph.livePasses()[index])
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {1, 1}});
        }
        if (!matchesContract(logical, schema))
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {1, 2}});
        }
        if (variant.schema_identity != toolchain::shaderSchemaIdentity(schema))
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {1, 3}});
        }
        const bool invalid_identity = !reference.isValid() || logical.shader_name != reference.canonicalName() ||
                                      logical.shader != shaderKey(reference);
        if (invalid_identity)
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {1, 4}});
        }
        const bool compute = std::holds_alternative<ComputeDescription>(description);
        const bool invalid_kind = compute ? logical.kind != EPassKind::COMPUTE : logical.kind != EPassKind::GRAPHICS;
        const bool invalid_stages =
            compute ? variant.stages.size() != 1 || variant.stages[0].stage != 4
                    : variant.stages.size() != 2 || variant.stages[0].stage != 1 || variant.stages[1].stage != 2;
        if (invalid_kind || invalid_stages)
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {2}});
        }
        for (const auto& stage : variant.stages)
        {
            const auto interface = toolchain::reflectPassInterface(stage.words);
            if (!interface || !supportedInterface(*interface, device))
            {
                return cxx::unexpected(RenderError{kUnsupported, {2, stage.stage}});
            }
        }
        if (!compute && !matchesAttachments(graph.identity(), logical, std::get<GraphicsDescription>(description)))
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {2, 1}});
        }
        if (!compute &&
            !matchesInterfaces(variant, std::get<GraphicsDescription>(description), device.properties().limits))
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {2, 2}});
        }
        const auto caps = queryLayoutCaps(device);
        auto layout = compileLayout(schema, assignment, owners, caps);
        if (!layout)
        {
            return cxx::unexpected(layout.error());
        }
        std::vector<toolchain::PassDescriptorLocation> locations;
        for (const auto& field : layout->identity().fields)
        {
            locations.push_back({field.field_index, field.set, field.binding});
        }
        std::vector<toolchain::ShaderStageBinary> relocated;
        for (const auto& stage : variant.stages)
        {
            auto patched = toolchain::relocatePassSpirv(stage.words, schema, locations, stage.stage);
            if (!patched)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {3, stage.stage}});
            }
            relocated.push_back({stage.stage, std::move(patched->words)});
        }
        std::vector<DescriptorSetLayout> sets;
        std::vector<VkDescriptorSetLayout> native_sets;
        for (const auto& set : layout->identity().sets)
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (const auto& binding : set.bindings)
            {
                const auto owner = std::find_if(
                    layout->identity().owners.begin(),
                    layout->identity().owners.end(),
                    [&](const auto& value) { return value.identity == binding.owner; }
                );
                const auto& field = owner->fields[binding.owner_field];
                bindings.push_back({binding.binding, field.type, field.count, nativeStages(field.stages), nullptr});
            }
            auto created = DescriptorSetLayout::create(device, bindings);
            if (!created)
            {
                return cxx::unexpected(created.error());
            }
            native_sets.push_back(created->native());
            sets.push_back(std::move(*created));
        }
        std::vector<VkPushConstantRange> ranges;
        for (const auto& range : layout->identity().push_ranges)
        {
            ranges.push_back({nativeStages(range.stages), range.offset, range.size});
        }
        auto pipeline_layout = PipelineLayout::create(device, native_sets, ranges);
        if (!pipeline_layout)
        {
            return cxx::unexpected(pipeline_layout.error());
        }
        std::vector<ShaderModule> modules;
        for (const auto& stage : relocated)
        {
            auto module = ShaderModule::create(device, stage.words);
            if (!module)
            {
                return cxx::unexpected(module.error());
            }
            modules.push_back(std::move(*module));
        }
        NativeProgramIdentity identity{
            graph.cacheIdentity(),
            pass,
            variant,
            layout->identity(),
            std::move(description),
            std::move(relocated),
            caps.vendor_id,
            caps.device_id,
            caps.driver_version,
            caps.api_version,
            device.dynamicRendering(),
            device.native()
        };
        if (compute)
        {
            auto pipeline = ComputePipeline::create(device, modules[0], *pipeline_layout);
            if (!pipeline)
            {
                return cxx::unexpected(pipeline.error());
            }
            return NativeShaderProgram{
                std::move(identity),
                std::move(sets),
                std::move(*pipeline_layout),
                std::move(modules),
                VPipeline{std::in_place_type<ComputePipeline>, std::move(*pipeline)}
            };
        }
        auto pipeline = GraphicsPipeline::create(
            device,
            modules[0],
            modules[1],
            *pipeline_layout,
            std::get<GraphicsDescription>(identity.pipeline)
        );
        if (!pipeline)
        {
            return cxx::unexpected(pipeline.error());
        }
        return NativeShaderProgram{
            std::move(identity),
            std::move(sets),
            std::move(*pipeline_layout),
            std::move(modules),
            VPipeline{std::in_place_type<GraphicsPipeline>, std::move(*pipeline)}
        };
    }

} // namespace lux::render::vulkan
