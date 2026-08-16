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
        // 异构查找:探测不构造 string,只有真要插入时才建键。
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end())
            return it->second;
        const FrameExtensionSlotId id = next_id_++;
        name_to_id_.emplace(name, id);
        return id;
    }

    FrameExtensionSlotId FrameExtensionRegistry::idOf(std::string_view name) const noexcept
    {
        auto it = name_to_id_.find(name);
        return it != name_to_id_.end() ? it->second : kInvalidExtSlot;
    }

} // namespace lux::render
