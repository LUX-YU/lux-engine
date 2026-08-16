// ============================================================================
//  ScriptRegistry.cpp — backend/factory registry + Cpp instance creation.
//  ScriptRegistry is a friend of ScriptBehavior, so it injects EntityHandle.
//  + World into a freshly built behavior before the ScriptSystem calls onCreate.
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>

#include <algorithm>   // cppScriptNames sort (A-5 manifest)
#include <cstdio>      // unregistered-behavior diagnostic (A-6)
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

    void ScriptRegistry::registerBackend(std::unique_ptr<IScriptBackend> backend)
    {
        if (backend)
            backends_.push_back(std::move(backend));
    }

    void ScriptRegistry::beginFrame(const ScriptContext& ctx)
    {
        for (const auto& backend : backends_)
            backend->beginFrame(ctx);
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
    ScriptRegistry::createInstanceFromAsset(
        lux::meta::EntityHandle entity,
        World& world,
                                            const lux::rdesc::Script&  desc,
                                            std::span<const std::byte> payload,
                                            std::string_view           cache_key) const
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
                std::fprintf(stderr,
                    "[ScriptRegistry] C++ behavior '%s' is not registered in "
                    "this binary — instance not created\n", cpp->behavior.c_str());
                return {};
            }
            const CppBehaviorOps& ops = it->second;

            void* state = ops.construct();
            if (!state)
                return {};
            auto* behavior   = static_cast<ScriptBehavior*>(state);
            behavior->self_  = entity;   // friend access — bind BEFORE onCreate
            behavior->world_ = &world;

            ScriptInstance inst(state, ops.destroy);
            inst.bind(ScriptEventRegistry::kOnCreate,  ops.on_create);
            inst.bind(ScriptEventRegistry::kOnUpdate,  ops.on_update);
            inst.bind(ScriptEventRegistry::kOnDestroy, ops.on_destroy);
            return inst;
        }

        for (const auto& backend : backends_)
        {
            if (auto inst = backend->createInstanceFromAsset(
                    entity, world, desc, payload, cache_key))
                return inst;
        }
        return {};   // no backend claims this kind
    }

    ScriptRegistry& scriptRegistry()
    {
        static ScriptRegistry instance;   // function-local static: safe init order
        return instance;
    }
} // namespace lux::ecs
