#include <algorithm>
#include <limits>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <spirv-tools/libspirv.h>

namespace lux::render::detail
{
    RenderResult<PipelineLayoutPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis
    ) noexcept
    {
        PipelineLayoutPlan plan;
        plan.passes.resize(graph.passes().size());
        for (const auto id : analysis.execution_order)
        {
            const auto& pass = graph.passes()[id.index];
            const PassInterface* input = nullptr;
            if (const auto* raster = std::get_if<RasterWork>(&pass.work))
            {
                input = &raster->interface;
            }
            else if (const auto* compute = std::get_if<ComputeWork>(&pass.work))
            {
                input = &compute->interface;
            }
            else
            {
                auto& output = plan.passes[id.index];
                const auto& transfer = std::get<TransferWork>(pass.work);
                for (const auto& operation : transfer.operations)
                {
                    if (const auto* fill = std::get_if<FillBuffer>(&operation))
                    {
                        if (!fill->value)
                        {
                            continue;
                        }
                        const auto binding = static_cast<std::uint32_t>(output.bindings.size());
                        output.bindings.push_back(
                            {0, binding, EDescriptorKind::TYPED_STORAGE_BUFFER, 1, {}, {}, 4, EShaderAccess::WRITE_ONLY}
                        );
                        output.uses.push_back(
                            {fill->destination.version.resource.index,
                             0,
                             binding,
                             EDescriptorKind::TYPED_STORAGE_BUFFER,
                             fill->destination.range,
                             binding}
                        );
                    }
                }
                output.private_bindings = output.bindings;
                continue;
            }
            const auto& shader = graph.programs()[input->program];
            auto reflected = shaderInterface(shader.interface);
            if (!reflected)
            {
                return cxx::unexpected(reflected.error());
            }
            auto& output = plan.passes[id.index];
            const auto& parameters = reflected->parameters;
            const bool invalid_parameters = input->parameters.values.size() != parameters.size ||
                                            parameters.runtime_records || parameters.size % 4 != 0 ||
                                            parameters.size > limits.pipelineLayout.rootConstantMaxSize;
            if (invalid_parameters)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            output.parameter_bytes = parameters.size;
            output.parameter_stages = shaderStages(reflected->parameter_stages);
            output.initial_parameters = input->parameters.values;
            const auto& instance = input->parameters.instance_layout;
            if (instance.size != 0)
            {
                if (plan.instance_parameters.size != 0 && plan.instance_parameters != instance)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                plan.instance_parameters = instance;
            }
            for (const auto& field : input->parameters.fields)
            {
                const auto destination =
                    std::ranges::find(parameters.fields, field.shader_field, &rdesc::ShaderMember::path);
                const bool missing = destination == parameters.fields.end() || field.source_field.empty();
                const bool duplicate = std::ranges::any_of(
                    output.parameters,
                    [&](const auto& old) { return old.destination.path == field.shader_field; }
                );
                if (missing || duplicate)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                output.parameters.push_back({field.resource.index, field.source_field, *destination});
            }
            output.bindings = std::move(reflected->bindings);
            if (output.bindings.size() != input->resources.size() + input->samplers.size())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            output.shared =
                std::ranges::any_of(output.bindings, [](const auto& binding) { return binding.count == 0; });
            output.private_set = output.shared ? 1u : 0u;
            std::vector<std::size_t> private_fields;
            std::vector<const ResourceParameter*> resources(output.bindings.size());
            std::vector<const SamplerParameter*> samplers(output.bindings.size());
            for (const auto& resource : input->resources)
            {
                resources[resource.field] = &resource;
            }
            for (const auto& sampler : input->samplers)
            {
                samplers[sampler.field] = &sampler;
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
                const bool buffer =
                    binding.kind == EDescriptorKind::STORAGE_BUFFER || binding.kind == EDescriptorKind::UNIFORM_BUFFER;
                const bool shared_kind =
                    binding.kind == EDescriptorKind::SAMPLED_IMAGE || binding.kind == EDescriptorKind::SAMPLER;
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
                    const bool image = std::holds_alternative<GraphImageDesc>(graph.resources()[index].description);
                    const bool wrong_kind = image == buffer;
                    const bool access_mismatch =
                        (binding.access != EShaderAccess::READ_ONLY && access.access == EGraphAccess::READ) ||
                        (binding.access == EShaderAccess::READ_ONLY && access.access != EGraphAccess::READ);
                    const bool invalid_shared =
                        binding.count == 0 && graph.resources()[index].origin != EGraphOrigin::IMPORTED;
                    if (wrong_kind || access_mismatch || invalid_shared)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                         access.whole_resource}
                    );
                }
                if (binding.count == 0)
                {
                    binding.set = 0;
                    binding.binding = binding.kind == EDescriptorKind::SAMPLER ? 1u : 0u;
                    output.uses.back().set = 0;
                    output.uses.back().binding = binding.binding;
                    if (plan.shared_bindings.empty())
                    {
                        for (const auto& range : DescriptorTable::layoutRanges(0, 0))
                        {
                            const auto kind = range.descriptorType == nri::DescriptorType::SAMPLER
                                                  ? EDescriptorKind::SAMPLER
                                                  : EDescriptorKind::SAMPLED_IMAGE;
                            plan.shared_bindings.push_back({0, range.baseRegisterIndex, kind, 0, {}, {}, 7});
                        }
                    }
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
        }
        std::vector<std::uint32_t> sampler_inputs;
        for (const auto& program : plan.passes)
        {
            for (const auto& use : program.uses)
            {
                if (use.sampler != kGraphInvalidIndex)
                {
                    sampler_inputs.push_back(use.sampler);
                }
            }
        }
        std::ranges::sort(sampler_inputs);
        sampler_inputs.erase(std::unique(sampler_inputs.begin(), sampler_inputs.end()), sampler_inputs.end());
        for (std::size_t i = 0; i < sampler_inputs.size(); ++i)
        {
            if (sampler_inputs[i] != i)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
        }
        return plan;
    }

    RenderResult<rdesc::SpirvArtifact> relocateShader(
        const rdesc::SpirvArtifact& original,
        std::span<const ShaderBinding> locations
    ) noexcept
    {
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
