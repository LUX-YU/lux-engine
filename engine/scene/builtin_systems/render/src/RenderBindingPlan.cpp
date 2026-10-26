#include <algorithm>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>

namespace lux::scene
{
    namespace
    {
        using InstalledInput = render::CapabilitiesInstalled::Input;

        render::RenderResult<std::optional<InstalledInput>> selectInput(
            const RenderBindingDeclaration& declaration,
            const RenderInputSelection& selection,
            std::span<const InstalledInput> candidates
        ) noexcept
        {
            const auto contract =
                std::ranges::find(declaration.inputs, selection.schema, &render::RenderPayloadContract::schema);
            if (contract == declaration.inputs.end() || contract->use != render::ERenderPayloadUse::RESOURCE_INPUT)
            {
                return cxx::unexpected(render::RenderError{render::kCommandContract});
            }
            const InstalledInput* found = nullptr;
            for (const auto& input : candidates)
            {
                const bool matches_schema = input.schema == selection.schema && input.version == contract->version;
                const bool matches_implementation =
                    selection.implementation.isNull() || input.destination.implementation == selection.implementation;
                const bool matches_index = !selection.index || input.destination.index == *selection.index;
                const bool matches_scope = selection.source != ERenderInputSource::VIEW ||
                                           std::holds_alternative<render::ViewRequest>(input.destination.scope);
                if (!matches_schema || !matches_implementation || !matches_index || !matches_scope)
                {
                    continue;
                }
                if (found && found->destination != input.destination)
                {
                    return cxx::unexpected(render::RenderError{render::kRuntimeUnsupported});
                }
                found = &input;
            }
            if (found)
            {
                return std::optional<InstalledInput>{*found};
            }
            if (contract->required || selection.index || !selection.implementation.isNull())
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeUnsupported});
            }
            return std::optional<InstalledInput>{};
        }
    } // namespace

    render::RenderResult<std::vector<render::CapabilitiesInstalled::Input>> resolveRenderBindingInputs(
        const RenderBindingDeclaration& declaration,
        std::span<const render::CapabilitiesInstalled::Input> scene_inputs,
        std::span<const render::CapabilitiesInstalled::Input> view_inputs,
        std::span<const render::CapabilitiesInstalled::Input> asset_inputs
    ) noexcept
    {
        std::vector<InstalledInput> result;
        for (const auto& selection : declaration.resource_inputs)
        {
            const auto candidates = selection.source == ERenderInputSource::ASSETS ? asset_inputs
                                    : selection.source == ERenderInputSource::VIEW ? view_inputs
                                                                                   : scene_inputs;
            auto selected = selectInput(declaration, selection, candidates);
            if (!selected)
            {
                return cxx::unexpected(selected.error());
            }
            if (*selected)
            {
                result.push_back(std::move(**selected));
            }
        }
        return result;
    }

    render::RenderResult<std::vector<render::CapabilitiesInstalled::Input>> selectRenderAssetInputs(
        const RenderBindingPlan& plan,
        std::span<const render::CapabilitiesInstalled::Input> asset_inputs
    ) noexcept
    {
        std::vector<InstalledInput> result;
        for (const auto& declaration : plan.declarations)
        {
            for (const auto& selection : declaration.resource_inputs)
            {
                if (selection.source != ERenderInputSource::ASSETS)
                {
                    continue;
                }
                auto selected = selectInput(declaration, selection, asset_inputs);
                if (!selected)
                {
                    return cxx::unexpected(selected.error());
                }
                if (*selected && std::ranges::none_of(
                                     result,
                                     [&](const auto& previous) noexcept
                                     {
                                         return previous.schema == (**selected).schema &&
                                                previous.destination == (**selected).destination;
                                     }
                                 ))
                {
                    result.push_back(std::move(**selected));
                }
            }
        }
        return result;
    }

    render::RenderResult<RenderBindingPlan> resolveRenderBindings(
        const RenderBindingDeclarations& available,
        const render::RenderCapability& capability
    ) noexcept
    {
        using namespace render;
        const auto declarations = available.declarations();
        const auto eligible = [&](const RenderBindingDeclaration& declaration) noexcept
        {
            const bool named =
                declaration.capabilities.empty() ||
                std::ranges::find(declaration.capabilities, capability.name) != declaration.capabilities.end();
            const bool complete = std::ranges::all_of(
                declaration.inputs,
                [&](const auto& provided) noexcept
                {
                    return !provided.required || std::ranges::any_of(
                                                     capability.inputs,
                                                     [&](const auto& requested) noexcept
                                                     { return RenderBindingDeclarations::matches(provided, requested); }
                                                 );
                }
            );
            return named && complete;
        };
        std::vector<bool> selected(declarations.size());
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            const auto& declaration = declarations[i];
            const bool matched = std::ranges::any_of(
                declaration.inputs,
                [&](const auto& provided) noexcept
                {
                    return std::ranges::any_of(
                        capability.inputs,
                        [&](const auto& requested) noexcept
                        { return RenderBindingDeclarations::matches(provided, requested); }
                    );
                }
            );
            selected[i] = eligible(declaration) && (matched || !declaration.capabilities.empty());
        }
        // Resolve explicit behavior dependencies once. No type/name discovery is performed in a frame.
        std::vector<std::vector<std::size_t>> predecessors(declarations.size());
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (std::size_t i = 0; i < declarations.size(); ++i)
            {
                if (!selected[i])
                {
                    continue;
                }
                const auto add_dependency = [&](std::size_t index) noexcept
                {
                    if (std::ranges::find(predecessors[i], index) == predecessors[i].end())
                    {
                        predecessors[i].push_back(index);
                    }
                    if (!selected[index])
                    {
                        selected[index] = true;
                        changed = true;
                    }
                };
                for (const auto dependency : declarations[i].dependencies)
                {
                    std::size_t found = declarations.size(), count = 0;
                    for (std::size_t j = 0; j < declarations.size(); ++j)
                    {
                        if (eligible(declarations[j]) && declarations[j].type == dependency &&
                            declarations[j].role == declarations[i].role)
                        {
                            found = j;
                            break;
                        }
                    }
                    if (found == declarations.size())
                    {
                        for (std::size_t j = 0; j < declarations.size(); ++j)
                        {
                            if (eligible(declarations[j]) && declarations[j].type == dependency)
                            {
                                found = j;
                                ++count;
                            }
                        }
                    }
                    if (found == declarations.size() || count > 1)
                    {
                        return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                    add_dependency(found);
                }
            }
        }
        // Aggregation consumes selected roles only; registering an alternative never activates it.
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            if (!selected[i])
            {
                continue;
            }
            for (const auto dependency : declarations[i].aggregate_dependencies)
            {
                const auto before = predecessors[i].size();
                for (std::size_t j = 0; j < declarations.size(); ++j)
                {
                    if (selected[j] && declarations[j].type == dependency)
                    {
                        predecessors[i].push_back(j);
                    }
                }
                if (before == predecessors[i].size())
                {
                    return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
            }
        }
        for (const auto& requested : capability.inputs)
        {
            const bool produced = std::ranges::any_of(
                declarations,
                [&](const auto& declaration) noexcept
                {
                    const auto i = static_cast<std::size_t>(&declaration - declarations.data());
                    return selected[i] && std::ranges::any_of(
                                              declaration.inputs,
                                              [&](const auto& provided) noexcept
                                              { return RenderBindingDeclarations::matches(provided, requested); }
                                          );
                }
            );
            if (requested.required && !produced)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            if (!selected[i])
            {
                continue;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (!selected[j])
                {
                    continue;
                }
                const bool shares_result = std::ranges::any_of(
                    declarations[i].results,
                    [&](auto type) noexcept
                    { return std::ranges::find(declarations[j].results, type) != declarations[j].results.end(); }
                );
                const bool single_writer = declarations[i].role == declarations[j].role ||
                                           !declarations[i].partitioned_results || !declarations[j].partitioned_results;
                if (shares_result && single_writer)
                {
                    return cxx::unexpected(render::RenderError{render::kCommandContract});
                }
            }
        }
        std::size_t effect_source = declarations.size();
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            if (!selected[i])
            {
                continue;
            }
            const auto& declaration = declarations[i];
            if (declaration.effect_inputs)
            {
                if (declaration.scope != ERenderOperationScope::SCENE)
                {
                    return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
                if (effect_source != declarations.size())
                {
                    return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
                effect_source = i;
            }
        }
        if (effect_source != declarations.size())
        {
            for (std::size_t i = 0; i < declarations.size(); ++i)
            {
                if (selected[i] && declarations[i].view_provider)
                {
                    predecessors[i].push_back(effect_source);
                }
            }
        }
        // Scene construction cannot borrow a not-yet-created View behavior.
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            if (selected[i] && declarations[i].scope == ERenderOperationScope::SCENE)
            {
                for (const auto predecessor : predecessors[i])
                {
                    if (declarations[predecessor].scope == ERenderOperationScope::VIEW)
                    {
                        return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                }
            }
        }
        RenderBindingPlan plan;
        plan.contracts = capability.inputs;
        std::vector<bool> published(declarations.size());
        auto remaining = std::ranges::count(selected, true);
        while (remaining != 0)
        {
            bool progressed = false;
            for (std::size_t i = 0; i < declarations.size(); ++i)
            {
                if (!selected[i] || published[i])
                {
                    continue;
                }
                if (!std::ranges::all_of(predecessors[i], [&](std::size_t j) noexcept { return published[j]; }))
                {
                    continue;
                }
                plan.declarations.push_back(declarations[i]);
                plan.declarations.back().role_index = static_cast<std::uint32_t>(i);
                for (const auto& input : declarations[i].data_inputs)
                {
                    const auto existing = std::ranges::find_if(
                        plan.data_inputs,
                        [&](const auto& value) noexcept { return value.name == input.name && value.kind == input.kind; }
                    );
                    if (existing == plan.data_inputs.end())
                    {
                        plan.data_inputs.push_back(input);
                    }
                    else if (existing->version != input.version)
                    {
                        return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                    else
                    {
                        existing->phase = std::min(existing->phase, input.phase);
                    }
                }
                published[i] = true;
                --remaining;
                progressed = true;
            }
            if (!progressed)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }
        return plan;
    }

    exec::task<render::RenderResult<RenderBindingPlan>> prepareRenderBindings(
        engine::render::RenderRequester& client,
        const RenderBindingDeclarations& available,
        RenderSystemConfiguration configuration
    ) noexcept
    {
        using namespace render;
        auto command = client.bind<ListCapabilities>();
        if (!command || !validRenderConfiguration(configuration))
        {
            co_return cxx::unexpected(RenderError{kCommandContract});
        }
        CapabilityQuery query{configuration.capability, configuration.capability_version, configuration.implementation};
        auto choices = co_await client.request(*command, query);
        if (choices.error.type != 0)
        {
            co_return cxx::unexpected(choices.error);
        }
        const bool unique = choices.implementations.size() == 1;
        const bool compatible =
            unique && choices.implementations.front().unavailable.type == 0 &&
            choices.implementations.front().implementation_version == configuration.implementation_version;
        if (!compatible)
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        const auto& support = choices.implementations.front();
        const auto capability =
            std::ranges::find(support.capabilities, configuration.capability, &RenderCapability::name);
        if (capability == support.capabilities.end() || capability->version != configuration.capability_version)
        {
            co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        auto plan = resolveRenderBindings(available, *capability);
        if (!plan)
        {
            co_return cxx::unexpected(plan.error());
        }
        auto validate = client.bind<ValidateCapabilityConfiguration>();
        if (!validate)
        {
            co_return cxx::unexpected(validate.error());
        }
        ValidateCapabilityConfiguration::Request validation{support.selection, configuration.configuration};
        const auto qualified = co_await client.request(*validate, validation);
        if (qualified.error.type != 0)
        {
            co_return cxx::unexpected(qualified.error);
        }
        plan->configuration = std::move(configuration);
        plan->implementation = support.selection;
        co_return std::move(*plan);
    }
} // namespace lux::scene
