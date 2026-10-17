#include "LayoutPlan.hpp"
#include <lux/engine/render/resource/Storage.hpp>
#include <algorithm>
#include <limits>
#include <spirv-tools/libspirv.h>

namespace lux::render::vulkan
{
    const ShaderBinding& sampledImageTableBinding() noexcept
    {
        // Complete shape of the existing device table, not a reflected subset or a Storage decision.
        static const ShaderBinding shape{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, 0, {}, 7};
        return shape;
    }

    RenderResult<LayoutPlan> planLayouts(
        const VulkanDevice& device, const RenderGraphDefinition& graph, const GraphAnalysis& analysis,
        std::span<const PassProgram* const> programs
    ) noexcept
    {
        LayoutPlan plan;
        plan.programs.resize(graph.passes().size());
        bool has_shared = false;
        std::size_t private_count = 0;
        for (const auto id : analysis.execution_order)
        {
            const auto* source = programs[id.index];
            if (!source) return cxx::unexpected(RenderError{kInvalidArgument, {id.index}});
            const auto& pass = graph.passes()[id.index];
            auto& output = plan.programs[id.index];
            if (source->shader.interface.bindings.size() > UINT32_MAX)
                return cxx::unexpected(RenderError{kCapacity});
            output.bindings = source->shader.interface.bindings;
            if (source->resources.size() != output.bindings.size())
                return cxx::unexpected(RenderError{kStorageContract});
            std::vector<std::size_t> private_fields;
            for (std::size_t field = 0; field < output.bindings.size(); ++field)
            {
                auto& binding = output.bindings[field];
                const bool invalid_identity = binding.name.empty() || !binding.stages || (binding.stages & ~7u);
                const bool invalid_shape = binding.kind > EDescriptorKind::STORAGE_BUFFER ||
                    (binding.kind == EDescriptorKind::STORAGE_BUFFER && binding.count != 1) ||
                    (binding.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY && binding.count != 0);
                if (invalid_identity || invalid_shape) return cxx::unexpected(RenderError{kStorageContract});
                for (std::size_t previous = 0; previous < field; ++previous)
                {
                    const auto& other = output.bindings[previous];
                    const bool duplicate_name = binding.name == other.name;
                    const bool duplicate_position = binding.set == other.set && binding.binding == other.binding;
                    if (duplicate_name || duplicate_position) return cxx::unexpected(RenderError{kStorageContract});
                }
                const ProgramResourceBinding* selected = nullptr;
                for (const auto& resource : source->resources)
                {
                    if (resource.shader_name != binding.name) continue;
                    if (selected) return cxx::unexpected(RenderError{kStorageContract});
                    selected = &resource;
                }
                if (!selected) return cxx::unexpected(RenderError{kStorageContract});
                const auto resource = std::ranges::find(graph.resources(), selected->resource_key, &GraphResource::key);
                if (resource == graph.resources().end()) return cxx::unexpected(RenderError{kStorageContract});
                const auto index = static_cast<std::uint32_t>(resource - graph.resources().begin());
                const auto usage = binding.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY ?
                    EGraphUse::SAMPLED : EGraphUse::STORAGE_READ;
                const bool declared = std::ranges::any_of(pass.uses, [&](const auto& use) {
                    return use.version.resource.index == index && use.access == EGraphAccess::READ && use.usage == usage;
                });
                const bool image = std::holds_alternative<GraphImageDesc>(resource->description);
                const bool wrong_kind = image != (binding.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY);
                const bool invalid_size = !image && binding.layout_bytes > std::get<GraphBufferDesc>(resource->description).size;
                if (!declared || wrong_kind || invalid_size) return cxx::unexpected(RenderError{kStorageContract});
                output.uses.push_back({index, 0, 0, binding.kind});
                if (image) has_shared = true;
                else private_fields.push_back(field);
            }
            // Field names, not author/provisional physical locations, determine private ordering.
            std::ranges::sort(private_fields, [&](auto a, auto b) {
                return output.bindings[a].name < output.bindings[b].name;
            });
            private_count = std::max(private_count, private_fields.size());
            for (std::uint32_t slot = 0; slot < private_fields.size(); ++slot)
            {
                const auto field = private_fields[slot];
                output.uses[field].binding = slot;
            }
            for (const auto& use : pass.uses)
            {
                const bool shader_read = use.usage == EGraphUse::SAMPLED || use.usage == EGraphUse::STORAGE_READ;
                if (shader_read && std::ranges::none_of(output.uses, [&](const auto& value) {
                    return value.resource == use.version.resource.index;
                })) return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        const auto set_count = unsigned(has_shared) + unsigned(private_count != 0);
        const auto& limits = device.properties().limits;
        const bool too_many = set_count > limits.maxBoundDescriptorSets ||
            private_count > limits.maxDescriptorSetStorageBuffers || private_count > UINT32_MAX;
        if (too_many) return cxx::unexpected(RenderError{kUnsupported});
        if (has_shared)
        {
            plan.shared_set = 0;
            plan.shared_bindings.push_back(sampledImageTableBinding());
        }
        if (private_count)
        {
            plan.private_set = unsigned(has_shared);
            plan.private_bindings.resize(private_count);
            for (std::uint32_t slot = 0; slot < private_count; ++slot)
                plan.private_bindings[slot] = {plan.private_set, slot, EDescriptorKind::STORAGE_BUFFER, 1, 0, {}, 0};
        }
        for (const auto id : analysis.execution_order)
        {
            auto& output = plan.programs[id.index];
            for (std::size_t field = 0; field < output.uses.size(); ++field)
            {
                auto& use = output.uses[field];
                auto& binding = output.bindings[field];
                use.set = use.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY ? plan.shared_set : plan.private_set;
                binding.set = use.set;
                binding.binding = use.binding;
                if (use.kind == EDescriptorKind::STORAGE_BUFFER)
                    plan.private_bindings[use.binding].stages |= binding.stages;
            }
        }
        for (const auto stage : {1u, 2u, 4u})
        {
            const auto count = std::ranges::count_if(plan.private_bindings, [&](const auto& value) {
                return (value.stages & stage) != 0;
            });
            if (count > limits.maxPerStageDescriptorStorageBuffers)
                return cxx::unexpected(RenderError{kUnsupported});
        }
        return plan;
    }

    RenderResult<SpirvArtifact> relocateShader(
        const SpirvArtifact& original, std::span<const ShaderBinding> locations
    ) noexcept
    {
        if (locations.size() != original.interface.bindings.size())
            return cxx::unexpected(RenderError{kStorageContract});
        auto result = original;
        result.interface.bindings.assign(locations.begin(), locations.end());
        // The compiler derived these locations from the validated field list. Only positions change.
        const bool unchanged = std::ranges::equal(original.interface.bindings, locations,
            [](const auto& a, const auto& b) { return a.set == b.set && a.binding == b.binding; });
        if (unchanged) return result;
        for (auto& stage : result.stages)
        {
            const auto words = std::span<const std::uint32_t>(stage.words);
            const bool invalid_header = words.size() < 5 || words[0] != 0x07230203 || words[1] < 0x00010000 ||
                words[1] > 0x00010600 || words[3] == 0 || words[3] > words.size() || words[4] != 0;
            if (invalid_header) return cxx::unexpected(RenderError{kInvalidArgument});
            struct Decoration final { std::size_t set = 0, binding = 0; bool variable = false; };
            std::vector<Decoration> decorations(words[3]);
            // Extracted from the audited SpirvRelocation.cpp boundary. Gather original word positions
            // before modifying any word, so cycles/swaps cannot rematch a previously patched location.
            for (std::size_t cursor = 5; cursor < words.size();)
            {
                const auto length = words[cursor] >> 16;
                const auto opcode = words[cursor] & 0xffff;
                const bool invalid_length = length == 0 || length > words.size() - cursor;
                if (invalid_length) return cxx::unexpected(RenderError{kInvalidArgument});
                const bool unsupported_decoration = opcode == 73 || opcode == 74 || opcode == 75 ||
                    opcode == 332 || opcode == 5632 || opcode == 5633;
                if (unsupported_decoration) return cxx::unexpected(RenderError{kUnsupported});
                if (opcode == 59) // OpVariable
                {
                    if (length < 4 || words[cursor + 2] >= decorations.size())
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    decorations[words[cursor + 2]].variable = true;
                }
                if (opcode == 71) // OpDecorate
                {
                    if (length < 3 || words[cursor + 1] >= decorations.size())
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    const auto kind = words[cursor + 2];
                    if (kind == 20) return cxx::unexpected(RenderError{kUnsupported}); // Aliased
                    if (kind == 33 || kind == 34)
                    {
                        if (length != 4) return cxx::unexpected(RenderError{kInvalidArgument});
                        auto& record = decorations[words[cursor + 1]];
                        auto& position = kind == 33 ? record.binding : record.set;
                        if (position) return cxx::unexpected(RenderError{kInvalidArgument});
                        position = cursor + 3;
                    }
                }
                cursor += length;
            }
            const auto validator = spvContextCreate(SPV_ENV_VULKAN_1_3);
            if (!validator) std::terminate();
            const auto valid = spvValidateBinary(validator, words.data(), words.size(), nullptr);
            spvContextDestroy(validator);
            if (valid == SPV_ERROR_OUT_OF_MEMORY) std::terminate();
            if (valid != SPV_SUCCESS) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto stage_bit = stage.stage == rdesc::EShaderType::VERTEX ? 1u :
                stage.stage == rdesc::EShaderType::FRAGMENT ? 2u : stage.stage == rdesc::EShaderType::COMPUTE ? 4u : 0u;
            std::vector<bool> seen(locations.size(), false);
            struct Patch final { std::size_t set_word, binding_word, field; };
            std::vector<Patch> patches;
            for (const auto& record : decorations)
            {
                if (!record.set && !record.binding) continue;
                const bool invalid_pair = !record.variable || !record.set || !record.binding;
                if (invalid_pair) return cxx::unexpected(RenderError{kInvalidArgument});
                const auto found = std::ranges::find_if(original.interface.bindings, [&](const auto& value) {
                    return value.set == words[record.set] && value.binding == words[record.binding] &&
                        (value.stages & stage_bit) != 0;
                });
                if (found == original.interface.bindings.end())
                    return cxx::unexpected(RenderError{kStorageMismatch});
                const auto field = static_cast<std::size_t>(found - original.interface.bindings.begin());
                if (seen[field]) return cxx::unexpected(RenderError{kStorageMismatch});
                seen[field] = true;
                patches.push_back({record.set, record.binding, field});
            }
            for (std::size_t field = 0; field < seen.size(); ++field)
                if (!seen[field] && (original.interface.bindings[field].stages & stage_bit) != 0)
                    return cxx::unexpected(RenderError{kStorageMismatch});
            for (const auto& patch : patches)
            {
                stage.words[patch.set_word] = locations[patch.field].set;
                stage.words[patch.binding_word] = locations[patch.field].binding;
            }
            // The pipeline boundary validates the relocated binary and complete declared ABI once.
        }
        return result;
    }
}
