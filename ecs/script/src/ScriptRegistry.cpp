// ============================================================================
//  ScriptRegistry.cpp — backend/factory registry + Cpp instance creation.
//  ScriptRegistry is a friend of ScriptBehavior, so it injects EntityHandle.
//  + World into a freshly built behavior before the ScriptSystem calls onCreate.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/log/Log.hpp>

#include <algorithm>   // cppScriptNames sort (A-5 manifest)
#include <string>
#include <utility>
#include <variant>     // CppBehaviorScript routing (A-6)

namespace lux::ecs
{
    void ScriptRegistry::registerCppScript(std::string_view name, CppBehaviorOps ops)
    {
        if (!name.empty() && ops.construct != nullptr && ops.destroy != nullptr)
            cpp_scripts_[std::string(name)] = ops;
    }

    bool ScriptRegistry::hasCppScript(std::string_view name) const
    {
        return cpp_scripts_.find(std::string(name)) != cpp_scripts_.end();
    }

    std::vector<std::string_view> ScriptRegistry::cppScriptNames() const
    {
        std::vector<std::string_view> names;
        names.reserve(cpp_scripts_.size());
        for (const auto& [name, ops] : cpp_scripts_)
            names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    ScriptInstance
    ScriptRegistry::createCppInstanceFromAsset(
        lux::ecs::EntityHandle entity,
        World& world,
        const lux::rdesc::Script& desc) const
    {
        // Cpp behaviors resolve HERE — the registry owns the ops table.
        // The asset (CppBehaviorScript) is the manifest entry naming a
        // LUX_REGISTER_SCRIPT behavior. Per ADR v2: construction goes
        // through the type's POOL, the shims are the registration template's
        // DEVIRTUALIZED direct calls, and only events T actually overrides
        // get bound — C++ follows the same "dispatch only to implementers"
        // contract as Lua.
        if (const auto* cpp = std::get_if<lux::rdesc::CppBehaviorScript>(&desc.body))
        {
            const auto it = cpp_scripts_.find(cpp->behavior);
            if (it == cpp_scripts_.end())
            {
                lux::log::error(
                    "ecs.script",
                    "C++ behavior '{}' is not registered in this binary; "
                    "instance not created",
                    cpp->behavior
                );
                return {};
            }
            const CppBehaviorOps& ops = it->second;

            void* state = ops.construct();
            if (!state)
                return {};
            ops.bind_context(state, entity, world);

            ScriptInstance inst(state, ops.destroy);
            inst.bind(
                ScriptEventRegistry::kOnCreate,
                BoundScriptCall{ops.on_create, state}
            );
            inst.bind(
                ScriptEventRegistry::kOnUpdate,
                BoundScriptCall{ops.on_update, state}
            );
            inst.bind(
                ScriptEventRegistry::kOnDestroy,
                BoundScriptCall{ops.on_destroy, state}
            );
            return inst;
        }
        return {};
    }

    ScriptRegistry& scriptRegistry()
    {
        static ScriptRegistry instance;   // function-local static: safe init order
        return instance;
    }
} // namespace lux::ecs
