#include <lux/engine/authoring/flowforge/TypedEntryCatalog.hpp>

#include <type_traits>

namespace lux::flowforge
{
    std::vector<TypedEntryNode> makeTypedEntryCatalog(
        const lux::simulation::SimulationDescription& description,
        lux::rdesc::EScriptModel model
    )
    {
        using namespace lux::simulation;
        const auto source = lux::authoring::makeScriptBindingTargetCatalog(
            description
        );
        std::vector<TypedEntryNode> result;
        for (const auto& entry : source)
        {
            if (entry.model != model)
                continue;
            TypedEntryNode node;
            node.cardinality = entry.cardinality;
            node.parameters = entry.parameters;
            node.returns = entry.returns;
            std::visit(
                [&](const auto& target)
                {
                    using Target = std::remove_cvref_t<decltype(target)>;
                    if constexpr (std::is_same_v<
                                      Target,
                                      SystemHookBindingTarget>)
                    {
                        node.kind = ETypedEntryKind::HOOK;
                        node.system_type = target.system_type.name;
                        node.system_instance = target.system_instance;
                        node.member = target.hook;
                    }
                    else if constexpr (std::is_same_v<
                                           Target,
                                           SystemEventBindingTarget>)
                    {
                        node.kind = ETypedEntryKind::EVENT;
                        node.system_type = target.system_type.name;
                        node.system_instance = target.system_instance;
                        node.member = target.event;
                        const auto event = description.findEvent(
                            target.system_instance,
                            target.event
                        );
                        node.event_target = event.target();
                    }
                    else
                    {
                        node.kind = ETypedEntryKind::LIFECYCLE;
                        switch (target.point)
                        {
                        case EBehaviorLifecyclePoint::CONSTRUCT:
                            node.member = "construct";
                            break;
                        case EBehaviorLifecyclePoint::START:
                            node.member = "start";
                            break;
                        case EBehaviorLifecyclePoint::STOP:
                            node.member = "stop";
                            break;
                        }
                    }
                },
                entry.target
            );
            result.push_back(std::move(node));
        }
        return result;
    }
}
