#include <algorithm>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/EffectCompilation.hpp>
#include <map>

namespace lux::render::runtime_detail
{
    RenderResult<RenderEffect> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const detail::EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::QueueType queue_type,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        std::span<const StorageNameView> storage_types,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes
    ) noexcept
    {
        const auto* input = &request;
        const auto count = input->definition.features.size();
        std::vector<FeatureBuildInput> selected;
        selected.reserve(count);
        for (std::size_t i = 0; i < input->definition.features.size(); ++i)
        {
            const auto& selection = input->definition.features[i];
            const auto* registration = registrations[i];
            selected.push_back(
                {&registration->descriptor,
                 &registration->graph,
                 selection.config ? &selection.config : nullptr,
                 selection.contribution_key}
            );
        }
        auto assembly = composeFeatures(
            selected,
            max_config_bytes,
            input->definition.connections,
            input->definition.outputs,
            input->definition.inputs
        );
        if (!assembly)
        {
            return cxx::unexpected(assembly.error());
        }
        // Installation owns these immutable names. Only this request's cold input is borrowed;
        // the Worker never searches the mutable Registry or acquires another registry owner.
        std::vector<bool> live_resources(assembly->definition.resources().size(), false);
        for (const auto& lifetime : assembly->analysis.lifetimes)
        {
            live_resources[lifetime.version.resource.index] = true;
        }
        for (auto parameter : assembly->analysis.parameter_inputs)
        {
            live_resources[parameter.index] = true;
        }
        const auto qualify = [&](const auto& group) noexcept
        {
            for (const auto& binding : group)
            {
                if (!live_resources[binding.resource.index])
                {
                    continue;
                }
                const auto name = binding.storage.type.name();
                const auto found = std::lower_bound(
                    storage_types.begin(),
                    storage_types.end(),
                    name,
                    [](StorageNameView type, std::string_view name) { return type.name() < name; }
                );
                if (found == storage_types.end() || found->name() != name)
                {
                    return false;
                }
            }
            return true;
        };
        const auto& external = assembly->external_bindings;
        const bool missing_factory = !qualify(external.global) || !qualify(external.scene) || !qualify(external.view) ||
                                     !qualify(external.attachment);
        if (missing_factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        auto compiled = detail::GraphCompiler::compile(
            device,
            nri_core,
            enabled,
            formats,
            vertex_limits,
            queue_type,
            sampled_image_capacity,
            sampler_capacity,
            assembly->definition,
            assembly->analysis,
            &input->description
        );
        if (!compiled)
        {
            return cxx::unexpected(compiled.error());
        }
        return RenderEffect{
            std::make_unique<detail::RenderPlan>(std::move(*compiled)),
            std::move(assembly->definition),
            std::move(assembly->analysis),
            std::move(assembly->external_bindings),
            std::move(assembly->arguments),
            std::move(assembly->bindings)
        };
    }

} // namespace lux::render::runtime_detail
