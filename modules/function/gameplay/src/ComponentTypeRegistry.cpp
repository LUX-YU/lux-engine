#include <lux/engine/gameplay/ComponentTypeRegistry.hpp>

namespace lux::gameplay
{
    // ──────────────────────────────────────────────────────────────────────
    // Magic-static singleton. Lives in `function::gameplay`'s shared library
    // so that every DLL linking gameplay (editor host, engine modules) sees
    // the same instance.
    // ──────────────────────────────────────────────────────────────────────
    ComponentTypeRegistry& ComponentTypeRegistry::instance() noexcept
    {
        static ComponentTypeRegistry s;
        return s;
    }

    void ComponentTypeRegistry::registerType(ComponentTypeEntry entry)
    {
        // Replace-on-duplicate: a later registration (e.g. an override
        // landing after a stub) takes precedence. Linear scan is fine —
        // total component-type count is in the dozens, not thousands.
        for (auto& existing : entries_)
        {
            if (existing.full_name == entry.full_name)
            {
                existing = entry;
                return;
            }
        }
        entries_.push_back(entry);
    }

    std::span<const ComponentTypeEntry> ComponentTypeRegistry::all() const noexcept
    {
        return std::span<const ComponentTypeEntry>(entries_.data(), entries_.size());
    }
} // namespace lux::gameplay
