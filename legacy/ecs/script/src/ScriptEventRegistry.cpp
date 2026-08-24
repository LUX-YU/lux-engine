// ============================================================================
//  ScriptEventRegistry.cpp — the process-domain event catalogue (ADR v2 §3.1).
// ============================================================================

#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>
#include <lux/engine/log/Log.hpp>

#include <utility>

namespace lux::ecs
{
    namespace
    {
        std::uint8_t abiKind(const lux::meta::RefType& type) noexcept
        {
            using lux::meta::EBaseType;
            switch (static_cast<EBaseType>(type.qtype.base))
            {
                case EBaseType::Bool:   return LUX_SCRIPT_VK_BOOL;
                case EBaseType::Int8:
                case EBaseType::Int16:
                case EBaseType::Int32:  return LUX_SCRIPT_VK_INT32;
                case EBaseType::Uint8:
                case EBaseType::Uint16:
                case EBaseType::Uint32: return LUX_SCRIPT_VK_UINT32;
                case EBaseType::Int64:  return LUX_SCRIPT_VK_INT64;
                case EBaseType::Uint64: return LUX_SCRIPT_VK_UINT64;
                case EBaseType::Float:  return LUX_SCRIPT_VK_FLOAT;
                case EBaseType::Double: return LUX_SCRIPT_VK_DOUBLE;
                case EBaseType::Record: return LUX_SCRIPT_VK_STRUCT_REF;
                default:                return LUX_SCRIPT_VK_VOID;
            }
        }
    }

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
            lux::log::error(
                "ecs.script",
                "event '{}' has {} parameters; dispatch limit is {}",
                name,
                params.size(),
                kMaxParams
            );
            return kInvalidScriptEvent;
        }

        const auto id = static_cast<ScriptEventId>(entries_.size());
        ScriptEventDesc event;
        event.name = std::string(name);
        event.params = std::move(params);
        event.abi_params.reserve(event.params.size());
        for (const auto& parameter : event.params)
        {
            if (!parameter.type)
                return kInvalidScriptEvent;
            lux_script_type_desc abi{};
            abi.name = parameter.type->name.data();
            abi.type_id = parameter.type->hash;
            abi.size = parameter.type->size;
            abi.align = 0;
            abi.kind = abiKind(*parameter.type);
            if (abi.kind == LUX_SCRIPT_VK_VOID)
                return kInvalidScriptEvent;
            event.abi_params.push_back(abi);
        }
        entries_.push_back(std::move(event));
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
