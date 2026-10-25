#include <algorithm>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>

namespace lux::scene
{
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
        for (const auto& input : capability->inputs)
        {
            if (input.required && !available.supports(input))
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }
        const auto declarations = available.declarations();
        std::vector<bool> selected(declarations.size());
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            const auto& declaration = declarations[i];
            selected[i] = declaration.effect_output && capability->kind == ERenderCapabilityKind::EFFECT;
            for (const auto& provided : declaration.inputs)
            {
                selected[i] = selected[i] || std::ranges::any_of(
                                                 capability->inputs,
                                                 [&](const auto& requested) noexcept
                                                 { return RenderBindingDeclarations::matches(provided, requested); }
                                             );
            }
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
                        if (declarations[j].type == dependency && declarations[j].role == declarations[i].role)
                        {
                            found = j;
                            break;
                        }
                    }
                    if (found == declarations.size())
                    {
                        for (std::size_t j = 0; j < declarations.size(); ++j)
                        {
                            if (declarations[j].type == dependency)
                            {
                                found = j;
                                ++count;
                            }
                        }
                    }
                    if (found == declarations.size() || count > 1)
                    {
                        co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                    add_dependency(found);
                }
                for (const auto dependency : declarations[i].aggregate_dependencies)
                {
                    bool found = false;
                    for (std::size_t j = 0; j < declarations.size(); ++j)
                    {
                        if (declarations[j].type == dependency)
                        {
                            add_dependency(j);
                            found = true;
                        }
                    }
                    if (!found)
                    {
                        co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
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
            for (const auto& provided : declaration.inputs)
            {
                const bool supplied = std::ranges::any_of(
                    capability->inputs,
                    [&](const auto& requested) noexcept
                    { return RenderBindingDeclarations::matches(provided, requested); }
                );
                if (!supplied && provided.required)
                {
                    co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
            }
            if (declaration.effect_inputs)
            {
                if (declaration.scope != ERenderOperationScope::SCENE)
                {
                    co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
                if (effect_source != declarations.size())
                {
                    co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
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
                if (selected[i] && declarations[i].effect_output)
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
                        co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                }
            }
        }
        RenderBindingPlan plan;
        plan.configuration = std::move(configuration);
        plan.implementation = support.selection;
        plan.contracts = capability->inputs;
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
                        co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
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
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }
        co_return plan;
    }
} // namespace lux::scene
