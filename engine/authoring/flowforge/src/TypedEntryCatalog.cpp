#include <lux/engine/authoring/flowforge/TypedEntryCatalog.hpp>

namespace lux::flowforge
{
    std::vector<TypedEntryNode> makeTypedEntryCatalog(
        const lux::simulation::SimulationDescription& description
    )
    {
        std::vector<TypedEntryNode> result;
        for (std::size_t system_index{};
             system_index < description.systemCount();
             ++system_index)
        {
            const auto system = description.systemAt(system_index);
            for (std::size_t hook_index{};
                 hook_index < system.hookPointCount();
                 ++hook_index)
            {
                const auto hook = system.hookPointAt(hook_index);
                TypedEntryNode node;
                node.kind = ETypedEntryKind::HOOK;
                node.system_type = system.type().name;
                node.system_instance = system.instanceName();
                node.member = hook.name();
                node.cardinality = hook.cardinality();
                node.parameters.reserve(hook.parameterCount());
                node.returns.reserve(hook.returnCount());
                for (std::size_t index{}; index < hook.parameterCount(); ++index)
                    node.parameters.push_back(hook.parameterAt(index));
                for (std::size_t index{}; index < hook.returnCount(); ++index)
                    node.returns.push_back(hook.returnAt(index));
                result.push_back(std::move(node));
            }
            for (std::size_t event_index{};
                 event_index < system.eventCount();
                 ++event_index)
            {
                const auto event = system.eventAt(event_index);
                TypedEntryNode node;
                node.kind = ETypedEntryKind::EVENT;
                node.system_type = system.type().name;
                node.system_instance = system.instanceName();
                node.member = event.name();
                node.event_target = event.target();
                if (!event.payloadSchemaName().empty())
                {
                    node.parameters.push_back(lux::script::ScriptSemanticType{
                        event.payloadSchemaHash(),
                        event.payloadSchemaName(),
                        lux::script::EScriptPassMode::VALUE});
                }
                result.push_back(std::move(node));
            }
        }
        return result;
    }
}
