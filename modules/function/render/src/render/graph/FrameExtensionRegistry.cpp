#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>

namespace lux::render
{
    FrameExtensionRegistry& FrameExtensionRegistry::instance() noexcept
    {
        static FrameExtensionRegistry s_instance;
        return s_instance;
    }

    FrameExtensionSlotId FrameExtensionRegistry::registerSlot(std::string_view name)
    {
        auto it = name_to_id_.find(std::string(name));
        if (it != name_to_id_.end())
            return it->second;
        const FrameExtensionSlotId id = next_id_++;
        name_to_id_.emplace(std::string(name), id);
        return id;
    }

    FrameExtensionSlotId FrameExtensionRegistry::idOf(std::string_view name) const noexcept
    {
        auto it = name_to_id_.find(std::string(name));
        return it != name_to_id_.end() ? it->second : kInvalidExtSlot;
    }

} // namespace lux::render
