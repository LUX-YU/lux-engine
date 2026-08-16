// ============================================================================
//  ScriptEventRegistry.cpp — the process-domain event catalogue (ADR v2 §3.1).
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>

#include <cstdio>
#include <utility>

namespace lux::ecs
{
    ScriptEventRegistry::ScriptEventRegistry()
    {
        // The lifecycle trio — ordinary entries, registered first so the id
        // constants hold. OnUpdate's dt is the one built-in payload.
        registerEvent("OnCreate", {});
        registerEvent("OnUpdate",
                      { ScriptEventParam{ &lux::meta::ref_type_of_v<float>, "dt" } });
        registerEvent("OnDestroy", {});
    }

    ScriptEventId ScriptEventRegistry::registerEvent(
        std::string_view name, std::vector<ScriptEventParam> params)
    {
        if (name.empty())
            return kInvalidScriptEvent;
        if (const auto it = by_name_.find(name); it != by_name_.end())
            return it->second;   // idempotent (double-registration is benign)
        if (params.size() > kMaxParams)
        {
            std::fprintf(stderr,
                "[ScriptEventRegistry] '%.*s': %zu params exceed the dispatch "
                "ceiling (%zu) — event not registered\n",
                static_cast<int>(name.size()), name.data(), params.size(),
                kMaxParams);
            return kInvalidScriptEvent;
        }

        const auto id = static_cast<ScriptEventId>(entries_.size());
        entries_.push_back(ScriptEventDesc{ std::string(name), std::move(params) });
        by_name_.emplace(entries_.back().name, id);   // key views the deque-stable string
        return id;
    }

    ScriptEventId ScriptEventRegistry::find(std::string_view name) const
    {
        const auto it = by_name_.find(name);
        return it == by_name_.end() ? kInvalidScriptEvent : it->second;
    }

    const ScriptEventDesc& ScriptEventRegistry::desc(ScriptEventId id) const
    {
        return entries_[id];
    }

    std::size_t ScriptEventRegistry::count() const noexcept
    {
        return entries_.size();
    }

    ScriptEventRegistry& scriptEventRegistry()
    {
        static ScriptEventRegistry instance;
        return instance;
    }
} // namespace lux::ecs
