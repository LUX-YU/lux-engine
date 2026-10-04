#include <lux/engine/editor/flowforge/FlowEnvironment.hpp>

namespace lux::editor::flowforge
{
    struct FlowEnvironment::Data final
    {
        std::shared_ptr<const void> code;
        std::vector<const lux::meta::RefType*> types;
        std::vector<const lux::meta::RefClass*> classes;
        std::vector<const lux::meta::RefFunction*> functions;
        std::vector<lux::flowforge::ScriptAbilityNodeDescription> abilities;
        std::vector<lux::script::ScriptEventSourceDescription> events;
        std::uint64_t version;
    };
    FlowEnvironment::FlowEnvironment(lux::flowforge::FlowSourceEnvironment env, std::uint64_t version)
        : data_(std::make_shared<const Data>(Data{
              std::move(env.code_lifetime),
              {env.types.begin(), env.types.end()},
              {env.classes.begin(), env.classes.end()},
              {env.functions.begin(), env.functions.end()},
              {env.abilities.nodes().begin(), env.abilities.nodes().end()},
              {env.events.begin(), env.events.end()},
              version
          }))
    {
    }
    lux::flowforge::FlowSourceEnvironment FlowEnvironment::view() const noexcept
    {
        return {
            data_->types,
            data_->classes,
            data_->functions,
            lux::flowforge::ScriptAbilityNodeCatalogView{data_->abilities},
            data_->events,
            data_
        };
    }
    std::uint64_t FlowEnvironment::version() const noexcept
    {
        return data_->version;
    }
} // namespace lux::editor::flowforge
