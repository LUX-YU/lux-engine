#include <algorithm>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>

namespace lux::scene
{
    render::RenderResult<void> resolveRenderBindingInputs(
        RenderBindingPlan& plan,
        std::span<const render::CapabilitiesInstalled::Input> scene_inputs,
        std::span<const render::CapabilitiesInstalled::Input> resource_inputs
    ) noexcept
    {
        using namespace render;
        for (auto& declaration : plan.declarations)
        {
            auto selected = std::move(declaration.destinations);
            std::vector<CapabilitiesInstalled::Input> resolved;
            for (const auto& contract : declaration.inputs)
            {
                if (contract.use != ERenderPayloadUse::RESOURCE_INPUT)
                {
                    continue;
                }
                const auto chosen = std::ranges::find(selected, contract.schema, &CapabilitiesInstalled::Input::schema);
                const CapabilitiesInstalled::Input* found = nullptr;
                bool ambiguous = false;
                const auto consider = [&](const auto& input) noexcept
                {
                    if (input.schema != contract.schema || input.version != contract.version)
                    {
                        return;
                    }
                    if (chosen != selected.end() && input.destination != chosen->destination)
                    {
                        return;
                    }
                    ambiguous |= found && found->destination != input.destination;
                    found = &input;
                };
                const bool from_assets =
                    std::ranges::find(declaration.asset_inputs, contract.schema) != declaration.asset_inputs.end();
                const auto candidates = from_assets ? resource_inputs : scene_inputs;
                for (const auto& input : candidates)
                {
                    consider(input);
                }
                if (ambiguous || (!found && (contract.required || chosen != selected.end())))
                {
                    return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
                if (found)
                {
                    resolved.push_back(*found);
                }
            }
            for (const auto& schema : declaration.asset_inputs)
            {
                const auto found = std::ranges::find(declaration.inputs, schema, &RenderPayloadContract::schema);
                const bool invalid_source =
                    found == declaration.inputs.end() || found->use != ERenderPayloadUse::RESOURCE_INPUT;
                if (invalid_source)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
            }
            for (const auto& chosen : selected)
            {
                const auto count = std::ranges::count(selected, chosen.schema, &CapabilitiesInstalled::Input::schema);
                const auto found = std::ranges::find(resolved, chosen.schema, &CapabilitiesInstalled::Input::schema);
                if (count != 1 || found == resolved.end() || chosen.version != found->version)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
            }
            declaration.destinations = std::move(resolved);
        }
        return {};
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
            // Signal observers of hidden results are connected after their producing binding.
            for (std::size_t j = 0; j < declarations.size(); ++j)
            {
                if (!selected[j] || i == j)
                {
                    continue;
                }
                for (const auto source : declaration.sources)
                {
                    if (std::ranges::find(declarations[j].results, source) != declarations[j].results.end())
                    {
                        predecessors[i].push_back(j);
                    }
                }
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
        plan->configuration = std::move(configuration);
        plan->implementation = support.selection;
        co_return std::move(*plan);
    }
} // namespace lux::scene
