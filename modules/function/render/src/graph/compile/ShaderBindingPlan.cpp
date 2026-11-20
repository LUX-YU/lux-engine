#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <algorithm>
#include <limits>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <map>
#include <spirv-tools/libspirv.h>

namespace lux::render::detail
{
    namespace
    {
        bool matchesConstantBlock(
            ConstantBlockContract contract, rdesc::EShaderTarget target, const rdesc::ShaderBlock& shader
        ) noexcept
        {
            const auto layout = contract.layout(target);
            const auto same_member = [](const ConstantMemberLayout& host, const rdesc::ShaderMember& device) noexcept
            {
                return host.path == device.path && host.offset == device.offset && host.scalar == device.scalar &&
                    host.rows == device.rows && host.columns == device.columns &&
                    host.matrix_stride == device.matrix_stride && host.matrix_order == device.matrix_order &&
                    std::ranges::equal(host.array_dimensions, device.array_dimensions);
            };
            const bool qualified_storage = contract.type.isValid() && layout.bytes == contract.bytes;
            return qualified_storage && shader.hasConstantLayoutEvidence() && layout.bytes == shader.size &&
                layout.alignment == shader.alignment &&
                std::ranges::equal(layout.members, shader.fields, same_member);
        }

        RenderResult<void> qualifyUniformBlock(
            const ResourceParameter& input,
            const ShaderBinding& binding,
            const GraphResource& resource,
            const nri::DeviceDesc& limits
        ) noexcept
        {
            const bool fixed_read = binding.kind == EDescriptorKind::UNIFORM_BUFFER && binding.count == 1 &&
                input.descriptor_count == 0 && input.elements.empty() && input.access.access == EGraphAccess::READ;
            const auto* range = std::get_if<GraphBufferRange>(&input.access.range);
            const auto* buffer = std::get_if<GraphBufferDesc>(&resource.description);
            const bool has_exact_range = range && buffer && !input.access.whole_resource &&
                range->size == input.uniform_constant.bytes;
            const auto target = limits.graphicsAPI == nri::GraphicsAPI::D3D12
                ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
            const bool aligned_type = binding.block.alignment <= input.uniform_constant.alignment;
            if (!fixed_read || !has_exact_range || !aligned_type ||
                !matchesConstantBlock(input.uniform_constant, target, binding.block))
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});

            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            const bool valid_offset = alignment != 0 && range->offset % alignment == 0;
            const bool inside_buffer = range->offset <= buffer->size && range->size <= buffer->size - range->offset;
            if (!valid_offset || !inside_buffer) return cxx::unexpected(RenderError{kGraphInvalidRange});
            const auto physical_size = limits.graphicsAPI == nri::GraphicsAPI::D3D12
                ? range->size + (alignment - range->size % alignment) % alignment
                : range->size;
            if (physical_size > limits.memory.constantBufferMaxRange)
                return cxx::unexpected(RenderError{kDeviceCapacity, {physical_size, limits.memory.constantBufferMaxRange}});
            return {};
        }

        enum class ED3DTable : std::uint8_t { SRV, UAV, CBV, SAMPLER };

        ED3DTable tableKind(nri::DescriptorType type) noexcept
        {
            switch (type)
            {
            case nri::DescriptorType::SAMPLER: return ED3DTable::SAMPLER;
            case nri::DescriptorType::CONSTANT_BUFFER: return ED3DTable::CBV;
            case nri::DescriptorType::STORAGE_TEXTURE:
            case nri::DescriptorType::STORAGE_BUFFER:
            case nri::DescriptorType::STORAGE_STRUCTURED_BUFFER: return ED3DTable::UAV;
            default: return ED3DTable::SRV;
            }
        }

        std::uint64_t d3dTableBytes(std::span<const nri::DescriptorSetDesc> sets) noexcept
        {
            std::uint64_t tables = 0;
            for (const auto& set : sets)
            {
                // The pinned NRI combines adjacent ranges of the same D3D range type and visibility per set.
                std::optional<std::pair<ED3DTable, nri::StageBits>> previous;
                for (const auto& range : std::span(set.ranges, set.rangeNum))
                {
                    if (range.descriptorType == nri::DescriptorType::MUTABLE) continue;
                    const auto stages = range.shaderStages;
                    const bool single_graphics = stages == nri::StageBits::VERTEX_SHADER ||
                        stages == nri::StageBits::FRAGMENT_SHADER;
                    const auto visibility = single_graphics ? stages : nri::StageBits::ALL;
                    const auto key = std::pair{tableKind(range.descriptorType), visibility};
                    if (previous != key)
                    {
                        ++tables;
                        previous = key;
                    }
                }
            }
            return tables * sizeof(std::uint32_t);
        }
    } // namespace

    RenderResult<void>
    addPrivateDescriptors(
        nri::DescriptorPoolDesc& pool,
        const PassBindings& program
    ) noexcept
    {
        if (program.private_bindings.empty()) return {};
        if (pool.descriptorSetMaxNum == UINT32_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        ++pool.descriptorSetMaxNum;
        for (const auto& binding : program.private_bindings)
        {
            auto* count = [&]() noexcept -> std::uint32_t*
            {
                switch (binding.kind)
                {
                case EDescriptorKind::TYPED_STORAGE_BUFFER: return &pool.storageBufferMaxNum;
                case EDescriptorKind::STORAGE_BUFFER: return binding.access == EShaderAccess::READ_ONLY ?
                    &pool.structuredBufferMaxNum : &pool.storageStructuredBufferMaxNum;
                case EDescriptorKind::UNIFORM_BUFFER: return &pool.constantBufferMaxNum;
                case EDescriptorKind::STORAGE_IMAGE: return &pool.storageTextureMaxNum;
                case EDescriptorKind::SAMPLED_IMAGE: return &pool.textureMaxNum;
                case EDescriptorKind::SAMPLER: return &pool.samplerMaxNum;
                }
                return nullptr;
            }();
            if (!count || binding.count > UINT32_MAX - *count)
                return cxx::unexpected(RenderError{kDeviceCapacity});
            *count += binding.count;
        }
        return {};
    }

    RenderResult<nri::PipelineLayout*>
    createPipelineLayout(
        nri::Device& device,
        const nri::CoreInterface& core,
        const PassBindings& program,
        std::span<const nri::DescriptorRangeDesc> local,
        const std::array<nri::DescriptorRangeDesc, 3>& shared,
        std::uint32_t color_count,
        std::uint32_t& max_colors
    ) noexcept
    {
        std::vector<nri::DescriptorSetDesc> sets;
        for (std::size_t kind = 0; kind < program.shared_sets.size(); ++kind)
        {
            const auto set = program.shared_sets[kind];
            if (set == kGraphInvalidIndex)
            {
                continue;
            }
            if (shared[kind].descriptorNum == 0)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            sets.push_back({set, &shared[kind], 1});
        }
        if (!local.empty())
        {
            sets.push_back({program.private_set, local.data(), static_cast<std::uint32_t>(local.size())});
        }
        const auto& limits = core.GetDeviceDesc(device);
        constexpr auto supported_stages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
        const bool is_d3d12 = limits.graphicsAPI == nri::GraphicsAPI::D3D12;
        if (program.parameter_bytes)
        {
            const auto stages = static_cast<std::uint32_t>(program.parameter_stages);
            const auto allowed = static_cast<std::uint32_t>(supported_stages);
            const bool invalid_range = program.parameter_bytes % sizeof(std::uint32_t) != 0;
            const bool invalid_stages = (stages & ~allowed) != 0;
            if (invalid_range || invalid_stages)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
        }
        const auto root_bytes = program.parameter_bytes + (is_d3d12 ? d3dTableBytes(sets) : 0);
        const auto root_limit = limits.pipelineLayout.rootConstantMaxSize;
        if (root_bytes > root_limit)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity, {root_bytes, root_limit}});
        }
        if (sets.size() > limits.pipelineLayout.descriptorSetMaxNum)
        {
            return cxx::unexpected(
                RenderError{kDeviceCapacity, {sets.size(), limits.pipelineLayout.descriptorSetMaxNum}}
            );
        }
        // Sampler, uniform buffer, storage buffer, sampled image/texel, storage image/texel.
        // The UAB limits include both UAB and ordinary sets; ordinary limits count only ordinary sets.
        const auto ordinary_sets = std::array{
            limits.descriptorSet.samplerMaxNum,
            limits.descriptorSet.constantBufferMaxNum,
            limits.descriptorSet.storageBufferMaxNum,
            limits.descriptorSet.textureMaxNum,
            limits.descriptorSet.storageTextureMaxNum
        };
        const auto indexed_sets = std::array{
            limits.descriptorSet.updateAfterSet.samplerMaxNum,
            limits.descriptorSet.updateAfterSet.constantBufferMaxNum,
            limits.descriptorSet.updateAfterSet.storageBufferMaxNum,
            limits.descriptorSet.updateAfterSet.textureMaxNum,
            limits.descriptorSet.updateAfterSet.storageTextureMaxNum
        };
        const auto ordinary_stages = std::array{
            limits.shaderStage.descriptorSamplerMaxNum,
            limits.shaderStage.descriptorConstantBufferMaxNum,
            limits.shaderStage.descriptorStorageBufferMaxNum,
            limits.shaderStage.descriptorTextureMaxNum,
            limits.shaderStage.descriptorStorageTextureMaxNum
        };
        const auto indexed_stages = std::array{
            limits.shaderStage.updateAfterSet.descriptorSamplerMaxNum,
            limits.shaderStage.updateAfterSet.descriptorConstantBufferMaxNum,
            limits.shaderStage.updateAfterSet.descriptorStorageBufferMaxNum,
            limits.shaderStage.updateAfterSet.descriptorTextureMaxNum,
            limits.shaderStage.updateAfterSet.descriptorStorageTextureMaxNum
        };
        constexpr auto stages =
            std::array{nri::StageBits::VERTEX_SHADER, nri::StageBits::FRAGMENT_SHADER, nri::StageBits::COMPUTE_SHADER};
        max_colors = UINT32_MAX;
        for (unsigned subset = 0; subset < 2; ++subset)
        {
            std::array<std::uint64_t, 5> totals{};
            std::array<std::array<std::uint64_t, 5>, 3> per_stage{};
            for (const auto& set : sets)
            {
                const bool uses_update_after_set = std::ranges::any_of(
                    std::span(set.ranges, set.rangeNum),
                    [](const nri::DescriptorRangeDesc& range) noexcept
                    { return (range.flags & nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET) != 0; }
                );
                if (subset == 0 && uses_update_after_set)
                {
                    continue;
                }
                for (const auto& range : std::span(set.ranges, set.rangeNum))
                {
                    const auto kind = range.descriptorType == nri::DescriptorType::SAMPLER           ? 0u
                                      : range.descriptorType == nri::DescriptorType::CONSTANT_BUFFER ? 1u
                                      : range.descriptorType == nri::DescriptorType::STRUCTURED_BUFFER ||
                                              range.descriptorType == nri::DescriptorType::STORAGE_STRUCTURED_BUFFER
                                          ? 2u
                                      : range.descriptorType == nri::DescriptorType::TEXTURE ||
                                              range.descriptorType == nri::DescriptorType::BUFFER
                                          ? 3u
                                          : 4u;
                    totals[kind] += range.descriptorNum;
                    for (std::size_t stage = 0; stage < stages.size(); ++stage)
                    {
                        if ((range.shaderStages & stages[stage]) != 0)
                        {
                            per_stage[stage][kind] += range.descriptorNum;
                        }
                    }
                }
            }
            for (std::size_t kind = 0; kind < totals.size(); ++kind)
            {
                const auto limit =
                    subset == 0 ? ordinary_sets[kind] : std::max(ordinary_sets[kind], indexed_sets[kind]);
                if (totals[kind] > limit)
                {
                    return cxx::unexpected(RenderError{kDeviceCapacity, {totals[kind], limit, kind}});
                }
            }
            for (std::size_t stage = 0; stage < stages.size(); ++stage)
            {
                std::uint64_t resources = 0;
                for (std::size_t kind = 0; kind < totals.size(); ++kind)
                {
                    const auto count = per_stage[stage][kind];
                    const auto limit =
                        subset == 0 ? ordinary_stages[kind] : std::max(ordinary_stages[kind], indexed_stages[kind]);
                    if (count > limit)
                    {
                        return cxx::unexpected(RenderError{kDeviceCapacity, {count, limit, kind}});
                    }
                    if (kind != 0)
                    {
                        resources += count;
                    }
                }
                const auto limit =
                    subset == 0
                        ? limits.shaderStage.resourceMaxNum
                        : std::max(limits.shaderStage.resourceMaxNum, limits.shaderStage.updateAfterSet.resourceMaxNum);
                const auto required = resources + (stage == 1 ? color_count : 0);
                if (required > limit)
                {
                    return cxx::unexpected(RenderError{kDeviceCapacity, {required, limit, stage}});
                }
                if (stage == 1)
                {
                    max_colors = std::min(max_colors, static_cast<std::uint32_t>(limit - resources));
                }
            }
        }
        const nri::RootConstantDesc parameters{0, program.parameter_bytes, program.parameter_stages};
        nri::PipelineLayoutDesc description{};
        description.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        description.rootRegisterSpace = is_d3d12 ? 255 : 0;
        description.shaderStages = supported_stages;
        description.descriptorSets = sets.data();
        description.descriptorSetNum = static_cast<std::uint32_t>(sets.size());
        description.rootConstants = program.parameter_bytes ? &parameters : nullptr;
        description.rootConstantNum = unsigned(program.parameter_bytes != 0);
        nri::PipelineLayout* layout = nullptr;
        const auto created = core.CreatePipelineLayout(device, description, layout);
        if (created != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        return layout;
    }

    RenderResult<PassBindings>
    planProgramLayout(
        const nri::DeviceDesc& limits,
        const rdesc::ShaderInfo& shader,
        const PassInterface& input,
        std::span<const GraphResource> graph_resources
    ) noexcept
    {
        auto reflected = shaderInterface(shader);
        if (!reflected)
        {
            return cxx::unexpected(reflected.error());
        }
        PassBindings output;
        const auto& parameters = reflected->parameters;
        const auto& authored = input.parameters;
        if (authored.inline_constant.layout)
        {
            const auto target = limits.graphicsAPI == nri::GraphicsAPI::D3D12
                ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
            const bool has_other_writers = authored.source != EParameterSource::INSTANCE ||
                authored.instance_layout.size != 0 || !authored.fields.empty() || !authored.device_fields.empty();
            const bool compatible = authored.inline_constant.bytes == authored.values.size() &&
                matchesConstantBlock(authored.inline_constant, target, parameters);
            if (has_other_writers || !compatible)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        const bool is_invalid_values = input.parameters.zero_fill_tail
            ? input.parameters.values.size() > parameters.size : input.parameters.values.size() != parameters.size;
        const bool invalid_parameters = is_invalid_values ||
                                        parameters.runtime_records || parameters.size % 4 != 0 ||
                                        parameters.size > limits.pipelineLayout.rootConstantMaxSize;
        if (invalid_parameters)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        output.parameter_bytes = parameters.size;
        output.parameter_stages = shaderStages(reflected->parameter_stages);
        output.initial_parameters = input.parameters.values;
        output.initial_parameters.resize(parameters.size);
        for (const auto& field : input.parameters.device_fields)
        {
            const auto destination = std::ranges::find(parameters.fields, field.shader_field, &rdesc::ShaderMember::path);
            if (destination == parameters.fields.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const bool is_invalid_source = field.source != EDeviceParameter::INDIRECT_FIRST_INSTANCE;
            const bool is_invalid_scalar = destination->scalar != rdesc::EDataScalar::UINT ||
                destination->rows != 1 || destination->columns != 1 || destination->array_stride != 0;
            const bool is_invalid_range = destination->offset > parameters.size ||
                parameters.size - destination->offset < sizeof(std::uint32_t);
            const bool is_duplicate = std::ranges::any_of(output.device_parameters,
                [&](const auto& old) { return old.first == destination->offset; });
            const bool is_invalid_field = is_invalid_source || is_invalid_scalar || is_invalid_range || is_duplicate;
            if (is_invalid_field) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            output.device_parameters.emplace_back(destination->offset, field.source);
        }
        for (const auto& field : input.parameters.fields)
        {
            const auto destination =
                std::ranges::find(parameters.fields, field.shader_field, &rdesc::ShaderMember::path);
            const bool missing = destination == parameters.fields.end() || field.source_field.empty();
            const bool duplicate = std::ranges::any_of(
                output.parameters,
                [&](const auto& old) { return old.destination.path == field.shader_field; }
            );
            const bool has_device_source = !missing && std::ranges::any_of(output.device_parameters,
                [&](const auto& item) { return item.first == destination->offset; });
            const bool is_invalid_projection = missing || duplicate || has_device_source;
            if (is_invalid_projection)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            output.parameters.push_back({field.resource.index, field.source_field, *destination});
        }
        output.bindings = std::move(reflected->bindings);
        if (output.bindings.size() != input.resources.size() + input.samplers.size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        std::vector<std::size_t> private_fields;
        std::vector<const ResourceParameter*> resources(output.bindings.size());
        std::vector<const SamplerParameter*> samplers(output.bindings.size());
        for (const auto& resource : input.resources)
        {
            const auto found = std::ranges::find(output.bindings, resource.shader_name, &ShaderBinding::name);
            if (found == output.bindings.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto field = static_cast<std::size_t>(found - output.bindings.begin());
            if (resources[field]) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            resources[field] = &resource;
        }
        for (const auto& sampler : input.samplers)
        {
            const auto found = std::ranges::find(output.bindings, sampler.shader_name, &ShaderBinding::name);
            if (found == output.bindings.end()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto field = static_cast<std::size_t>(found - output.bindings.begin());
            if (samplers[field] || resources[field]) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            samplers[field] = &sampler;
        }
        for (std::size_t field = 0; field < output.bindings.size(); ++field)
        {
            const auto* resource = resources[field];
            const auto& binding = output.bindings[field];
            const bool shared = binding.count == 0 && (!resource || resource->descriptor_count == 0);
            if (shared)
            {
                const auto kind = binding.kind == EDescriptorKind::SAMPLED_IMAGE ? 0u
                                  : binding.kind == EDescriptorKind::SAMPLER     ? 1u
                                                                                 : 2u;
                output.shared_sets[kind] = 0;
            }
        }
        for (auto& set : output.shared_sets)
        {
            if (set != kGraphInvalidIndex)
            {
                set = output.private_set++;
            }
        }
        for (std::size_t field = 0; field < output.bindings.size(); ++field)
        {
            auto& binding = output.bindings[field];
            if (binding.kind == EDescriptorKind::SAMPLER && binding.count != 0)
            {
                const auto* selected = samplers[field];
                if (!selected || selected->inputs.size() != binding.count)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                for (std::uint32_t element = 0; element < binding.count; ++element)
                {
                    if (selected->inputs[element] == kGraphInvalidIndex)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                    output.uses.push_back(
                        {kGraphInvalidIndex,
                         output.private_set,
                         0,
                         binding.kind,
                         {},
                         static_cast<std::uint32_t>(field),
                         element,
                         selected->inputs[element]}
                    );
                    output.uses.back().comparison_sampler = selected->comparison;
                }
                private_fields.push_back(field);
                continue;
            }
            const auto* selected = resources[field];
            const bool invalid = !selected || binding.name.empty() || !binding.stages;
            if (invalid)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            if (selected->uniform_constant.layout)
            {
                const auto& source = graph_resources[selected->access.version.resource.index];
                if (auto valid = qualifyUniformBlock(*selected, binding, source, limits); !valid)
                    return cxx::unexpected(valid.error());
            }
            if (selected->descriptor_count)
            {
                const bool invalid_array = binding.count != 0 || binding.kind != EDescriptorKind::SAMPLED_IMAGE ||
                                           selected->descriptor_count != selected->elements.size() + 1;
                if (invalid_array)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                binding.count = selected->descriptor_count;
            }
            const bool buffer =
                binding.kind == EDescriptorKind::STORAGE_BUFFER || binding.kind == EDescriptorKind::UNIFORM_BUFFER;
            const bool shared_kind =
                binding.kind == EDescriptorKind::SAMPLED_IMAGE || binding.kind == EDescriptorKind::SAMPLER ||
                (binding.kind == EDescriptorKind::STORAGE_BUFFER && binding.access == EShaderAccess::READ_ONLY);
            const bool wrong_count =
                binding.count == 0 ? !selected->elements.empty() : selected->elements.size() + 1 != binding.count;
            if (wrong_count || (binding.count == 0 && !shared_kind))
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            for (std::uint32_t element = 0; element <= selected->elements.size(); ++element)
            {
                const auto& access = element == 0 ? selected->access : selected->elements[element - 1];
                const auto index = access.version.resource.index;
                const bool image = std::holds_alternative<GraphImageDesc>(graph_resources[index].description);
                const bool wrong_kind = image == buffer;
                const bool access_mismatch =
                    (binding.access != EShaderAccess::READ_ONLY && access.access == EGraphAccess::READ) ||
                    (binding.access == EShaderAccess::READ_ONLY && access.access != EGraphAccess::READ);
                const bool invalid_shared =
                    binding.count == 0 && graph_resources[index].origin != EGraphOrigin::IMPORTED;
                if (wrong_kind || access_mismatch || invalid_shared)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                if (image && binding.kind != EDescriptorKind::SAMPLER)
                {
                    const auto& range = std::get<GraphImageRange>(access.range);
                    const bool invalid_shape = !binding.image_arrayed && range.layer_count != 1;
                    const bool invalid_depth = binding.image_depth && range.aspects != 2;
                    if (invalid_shape || invalid_depth)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    }
                }
                output.uses.push_back(
                    {index,
                     output.private_set,
                     0,
                     binding.kind,
                     access.range,
                     static_cast<std::uint32_t>(field),
                     element,
                     kGraphInvalidIndex,
                     access.whole_resource,
                     access.access == EGraphAccess::READ && access.fallback ? access.fallback->resource.index
                                                                            : kGraphInvalidIndex}
                );
            }
            if (binding.count == 0)
            {
                const auto kind = binding.kind == EDescriptorKind::SAMPLED_IMAGE ? 0u
                                  : binding.kind == EDescriptorKind::SAMPLER     ? 1u
                                                                                 : 2u;
                binding.set = output.shared_sets[kind];
                binding.binding = 0;
                output.uses.back().set = binding.set;
                output.uses.back().binding = binding.binding;
            }
            else
            {
                private_fields.push_back(field);
            }
        }
        // Legacy LayoutAllocator: private identity belongs to this program, shared identity
        // uses the complete device table. Equal provisional binding numbers never merge programs.
        std::ranges::sort(
            private_fields,
            [&](auto a, auto b) { return output.bindings[a].name < output.bindings[b].name; }
        );
        for (auto field : private_fields)
        {
            auto& binding = output.bindings[field];
            binding.set = output.private_set;
            binding.binding = static_cast<std::uint32_t>(output.private_bindings.size());
            for (auto& use : output.uses)
            {
                if (use.field == field)
                {
                    use.set = binding.set;
                    use.binding = binding.binding;
                }
            }
            output.private_bindings.push_back(binding);
        }
        return output;
    }

    RenderResult<PassBindings>
    planRasterLayout(
        const nri::DeviceDesc& limits,
        const rdesc::ShaderInfo& shader,
        const RasterPassTemplate& stage
    ) noexcept
    {
        auto reflected = shaderInterface(shader);
        if (!reflected)
        {
            return cxx::unexpected(reflected.error());
        }
        PassInterface input;
        input.parameters = stage.parameters;
        std::vector<GraphResource> resources(stage.inputs.size());
        for (std::uint32_t field = 0; field < reflected->bindings.size(); ++field)
        {
            const auto& binding = reflected->bindings[field];
            const auto slot = std::ranges::find(stage.inputs, binding.name, &RasterPassTemplate::Input::name);
            const bool undeclared = slot == stage.inputs.end() || !slot->kind;
            const bool writable = binding.access != EShaderAccess::READ_ONLY;
            const bool wrong_stages = !undeclared && (binding.stages & ~slot->stages) != 0;
            if (undeclared || writable || wrong_stages)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            const auto kind = [&]() noexcept
            {
                switch (binding.kind)
                {
                case EDescriptorKind::SAMPLED_IMAGE:
                    return rdesc::EDescriptorType::SAMPLED_IMAGE;
                case EDescriptorKind::SAMPLER:
                    return rdesc::EDescriptorType::SAMPLER;
                case EDescriptorKind::UNIFORM_BUFFER:
                    return rdesc::EDescriptorType::UNIFORM_BUFFER;
                default:
                    return rdesc::EDescriptorType::STORAGE_BUFFER;
                }
            }();
            if (kind != *slot->kind || binding.kind == EDescriptorKind::STORAGE_IMAGE)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            const auto index = static_cast<std::uint32_t>(slot - stage.inputs.begin());
            auto& resource = resources[index];
            resource.key = slot->name;
            resource.origin = EGraphOrigin::IMPORTED;
            const bool image =
                binding.kind == EDescriptorKind::SAMPLED_IMAGE || binding.kind == EDescriptorKind::SAMPLER;
            const VGraphRange range =
                image ? VGraphRange{GraphImageRange{binding.image_depth ? 2u : 1u, 0, 1, 0, 1}}
                      : VGraphRange{GraphBufferRange{0, std::max<std::uint32_t>(binding.block.size, 4)}};
            resource.description = image ? VGraphResourceDesc{GraphImageDesc{}}
                                         : VGraphResourceDesc{GraphBufferDesc{std::get<GraphBufferRange>(range).size}};
            ResourceAccess access{{GraphResourceId{index}, 0}, range};
            access.usage = slot->usage;
            access.whole_resource = true;
            if (binding.kind == EDescriptorKind::SAMPLER && binding.count != 0)
            {
                SamplerParameter sampler{binding.name};
                sampler.field = field;
                sampler.comparison = slot->comparison_sampler;
                sampler.inputs.assign(binding.count, index);
                input.samplers.push_back(std::move(sampler));
            }
            else
            {
                ResourceParameter parameter{binding.name, access};
                parameter.field = field;
                if (binding.count > 1)
                {
                    parameter.elements.assign(binding.count - 1, access);
                }
                input.resources.push_back(std::move(parameter));
            }
        }
        // Only slot shapes are projected above. Placement, shared arrays, parameter ABI and
        // relocation continue through the very same algorithm as fixed Graph programs.
        return planProgramLayout(limits, shader, input, resources);
    }

    RenderResult<ShaderBindingPlan>
    planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        std::span<const std::uint32_t> parameter_groups,
        bool device_addresses,
        std::span<const InstanceDataRecipe* const> instance_data
    ) noexcept
    {
        ShaderBindingPlan plan;
        plan.passes.resize(graph.passes().size());
        const auto regionFor = [&](std::uint32_t group, EParameterSource source)
        {
            const auto found = std::ranges::find_if(plan.parameter_regions, [&](const auto& region)
            { return region.group == group && region.source == source; });
            const auto index = static_cast<std::uint32_t>(found - plan.parameter_regions.begin());
            if (found == plan.parameter_regions.end()) plan.parameter_regions.push_back({{}, group, 0, {}, source});
            return index;
        };
        for (const auto id : analysis.execution_order)
        {
            const auto& pass = graph.passes()[id.index];
            const auto* input = &pass.interface;
            auto& output = plan.passes[id.index];
            for (const auto& write : input->typed_buffer_writes)
            {
                const auto binding = static_cast<std::uint32_t>(output.bindings.size());
                output.bindings.push_back({
                    .binding = binding,
                    .kind = EDescriptorKind::TYPED_STORAGE_BUFFER,
                    .count = 1,
                    .stages = 4,
                    .access = EShaderAccess::WRITE_ONLY,
                    .shader_register = binding
                });
                output.uses.push_back({write.version.resource.index, 0, binding,
                    EDescriptorKind::TYPED_STORAGE_BUFFER, write.range, binding});
            }
            output.private_bindings = output.bindings;
            const auto& parameters = input->parameters;
            const auto group = parameter_groups.empty() ? 0u : parameter_groups[id.index];
            const auto region = regionFor(group, parameters.source);
            const auto& instance = parameters.instance_layout;
            auto& region_layout = plan.parameter_regions[region].layout;
            if (instance.size != 0)
            {
                if (region_layout.size != 0 && region_layout != instance)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                region_layout = instance;
            }
            plan.passes[id.index].instance_region = region;
            if (!input || input->program == kGraphInvalidIndex)
            {
                if (parameters.inline_constant.layout)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                // Dynamic programs receive layouts when prepared; the pass retains only instance input data.
                plan.passes[id.index].initial_parameters = parameters.values;
                continue;
            }
            const auto target = limits.graphicsAPI == nri::GraphicsAPI::D3D12
                ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
            const auto* shader = graph.programs()[input->program].find(target, device_addresses);
            if (!shader) return cxx::unexpected(RenderError{kDeviceUnsupported});
            auto program = planProgramLayout(limits, shader->interface, *input, graph.resources());
            if (!program)
            {
                return cxx::unexpected(program.error());
            }
            program->instance_region = region;
            plan.passes[id.index] = std::move(*program);
        }
        // A CPU preparation consumer can read a shared snapshot even without a shader using it.
        for (const auto* recipe : instance_data)
        {
            if (!recipe->parameter_bytes || recipe->parameter_source == EParameterSource::INSTANCE) continue;
            const auto region = regionFor(recipe->parameter_group, recipe->parameter_source);
            auto& layout = plan.parameter_regions[region].layout;
            auto canonical = viewParameterLayout();
            const bool incompatible_layout = layout.size != 0 && layout != canonical;
            if (incompatible_layout || recipe->parameter_bytes > canonical.size)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            layout = std::move(canonical);
        }
        // The caller's byte block contains only local regions. Shared View copies
        // use the same backing allocation, after the local prefix.
        for (const bool local : {true, false})
        {
            for (auto& region : plan.parameter_regions)
            {
                const bool local_source = region.source == EParameterSource::INSTANCE;
                if (local_source != local) continue;
                region.offset = plan.instance_parameter_bytes;
                const auto extent = (std::uint64_t(region.layout.size) + 15u) & ~std::uint64_t{15};
                if (extent > UINT32_MAX - plan.instance_parameter_bytes)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                plan.instance_parameter_bytes += static_cast<std::uint32_t>(extent);
            }
            if (local) plan.local_parameter_bytes = plan.instance_parameter_bytes;
        }
        for (auto& program : plan.passes)
        {
            for (auto& mapping : program.parameters)
            {
                if (mapping.resource != kGraphInvalidIndex)
                {
                    continue;
                }
                const auto& region = plan.parameter_regions[program.instance_region];
                const auto& fields = region.layout.fields;
                const auto field = std::ranges::find(fields, mapping.source_field, &rdesc::ShaderMember::path);
                if (field == fields.end())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto& destination = mapping.destination;
                const bool mismatch = field->scalar != destination.scalar || field->rows != destination.rows ||
                                      field->columns != destination.columns ||
                                      field->matrix_stride != destination.matrix_stride ||
                                      field->array_stride != destination.array_stride;
                const auto bytes = std::uint64_t(field->rows) * 4 +
                                   (field->columns ? std::uint64_t(field->columns - 1) * field->matrix_stride : 0);
                const bool invalid = !bytes || bytes > UINT32_MAX ||
                                     std::uint64_t(field->offset) + bytes > region.layout.size ||
                                     std::uint64_t(destination.offset) + bytes > program.parameter_bytes;
                if (mismatch || invalid)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                mapping.source_offset = region.offset + field->offset;
            }
        }
        std::map<std::uint32_t, bool> sampler_contracts;
        for (const auto& program : plan.passes)
        {
            for (const auto& use : program.uses)
            {
                if (use.sampler == kGraphInvalidIndex)
                {
                    continue;
                }
                auto [entry, inserted] = sampler_contracts.emplace(use.sampler, use.comparison_sampler);
                if (!inserted && entry->second != use.comparison_sampler)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
        }
        // Dead passes can leave holes in the declared sampler inputs. Runtime validates the
        // declarations at assembly; a standalone instance binds the remaining original slots.
        return plan;
    }

    RenderResult<rdesc::ShaderProgramArtifact>
    relocateShader(const rdesc::ShaderProgramArtifact& original, std::span<const ShaderBinding> locations) noexcept
    {
        if (original.target == rdesc::EShaderTarget::DXIL)
        {
            // Physical locations are carried by descriptor ranges in the same Pipeline layout.
            // A DXIL container and its own reflection remain unchanged.
            return original;
        }
        std::vector<const rdesc::EDescriptorBindingInfo*> bindings;
        for (const auto& set : original.interface.sets)
        {
            for (const auto& binding : set.bindings)
            {
                bindings.push_back(&binding);
            }
        }
        if (locations.size() != bindings.size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        auto result = original;
        result.interface.sets.clear();
        std::size_t index = 0;
        for (const auto& group : original.interface.sets)
        {
            for (auto binding : group.bindings)
            {
                binding.set = locations[index].set;
                binding.binding = locations[index++].binding;
                auto destination =
                    std::ranges::find(result.interface.sets, binding.set, &rdesc::DescriptorSetLayoutInfo::set);
                if (destination == result.interface.sets.end())
                {
                    result.interface.sets.emplace_back(binding.set);
                    destination = std::prev(result.interface.sets.end());
                }
                destination->bindings.push_back(std::move(binding));
            }
        }
        // The compiler derived these locations from the validated field list. Only positions change.
        const bool unchanged = std::ranges::equal(
            bindings,
            locations,
            [](const auto* a, const auto& b) { return a->set == b.set && a->binding == b.binding; }
        );
        if (unchanged)
        {
            return result;
        }
        // DXIL register assignments are fixed by target compilation. Never treat its container as SPIR-V.
        if (original.target != rdesc::EShaderTarget::SPIRV)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        for (auto& stage : result.stages)
        {
            const auto words = std::span<const std::uint32_t>(stage.words);
            const bool invalid_header = words.size() < 5 || words[0] != 0x07230203 || words[1] < 0x00010000 ||
                                        words[1] > 0x00010600 || words[3] == 0 || words[3] > words.size() ||
                                        words[4] != 0;
            if (invalid_header)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }

            struct Decoration final
            {
                std::size_t set = 0, binding = 0;
                bool variable = false;
            };

            std::vector<Decoration> decorations(words[3]);
            // Extracted from the audited SpirvRelocation.cpp boundary. Gather original word positions
            // before modifying any word, so cycles/swaps cannot rematch a previously patched location.
            for (std::size_t cursor = 5; cursor < words.size();)
            {
                const auto length = words[cursor] >> 16;
                const auto opcode = words[cursor] & 0xffff;
                const bool invalid_length = length == 0 || length > words.size() - cursor;
                if (invalid_length)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const bool unsupported_decoration =
                    opcode == 73 || opcode == 74 || opcode == 75 || opcode == 332 || opcode == 5632 || opcode == 5633;
                if (unsupported_decoration)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                if (opcode == 59) // OpVariable
                {
                    if (length < 4 || words[cursor + 2] >= decorations.size())
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    decorations[words[cursor + 2]].variable = true;
                }
                if (opcode == 71) // OpDecorate
                {
                    if (length < 3 || words[cursor + 1] >= decorations.size())
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto kind = words[cursor + 2];
                    if (kind == 20)
                    {
                        return cxx::unexpected(RenderError{kDeviceUnsupported}); // Aliased
                    }
                    if (kind == 33 || kind == 34)
                    {
                        if (length != 4)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        auto& record = decorations[words[cursor + 1]];
                        auto& position = kind == 33 ? record.binding : record.set;
                        if (position)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
                        position = cursor + 3;
                    }
                }
                cursor += length;
            }
            const auto validator = spvContextCreate(SPV_ENV_VULKAN_1_3);
            if (!validator)
            {
                std::terminate();
            }
            const auto valid = spvValidateBinary(validator, words.data(), words.size(), nullptr);
            spvContextDestroy(validator);
            if (valid == SPV_ERROR_OUT_OF_MEMORY)
            {
                std::terminate();
            }
            if (valid != SPV_SUCCESS)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const auto stage_bit = stage.stage == rdesc::EShaderType::VERTEX     ? 1u
                                   : stage.stage == rdesc::EShaderType::FRAGMENT ? 2u
                                   : stage.stage == rdesc::EShaderType::COMPUTE  ? 4u
                                                                                 : 0u;
            std::vector<bool> seen(locations.size(), false);

            struct Patch final
            {
                std::size_t set_word, binding_word, field;
            };

            std::vector<Patch> patches;
            for (const auto& record : decorations)
            {
                if (!record.set && !record.binding)
                {
                    continue;
                }
                const bool invalid_pair = !record.variable || !record.set || !record.binding;
                if (invalid_pair)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto found = std::ranges::find_if(
                    bindings,
                    [&](const auto* value)
                    {
                        return value->set == words[record.set] && value->binding == words[record.binding] &&
                               (value->stages & stage_bit) != 0;
                    }
                );
                if (found == bindings.end())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto field = static_cast<std::size_t>(found - bindings.begin());
                if (seen[field])
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                seen[field] = true;
                patches.push_back({record.set, record.binding, field});
            }
            for (std::size_t field = 0; field < seen.size(); ++field)
            {
                if (!seen[field] && (bindings[field]->stages & stage_bit) != 0)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            for (const auto& patch : patches)
            {
                stage.words[patch.set_word] = locations[patch.field].set;
                stage.words[patch.binding_word] = locations[patch.field].binding;
            }
            // The pipeline boundary validates the relocated binary and complete declared ABI once.
        }
        return result;
    }
} // namespace lux::render::detail
