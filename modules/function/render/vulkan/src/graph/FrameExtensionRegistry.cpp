#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <string>

namespace lux::render
{
    FrameExtensionRegistry& FrameExtensionRegistry::instance() noexcept
    {
        static FrameExtensionRegistry registry;
        return registry;
    }

    FrameExtensionResult FrameExtensionRegistry::registerSlot(std::string_view name) noexcept
    {
        const auto is_letter = [](char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        const bool has_valid_start = !name.empty() && (is_letter(name.front()) || name.front() == '_');
        if (!has_valid_start)
        {
            return lux::cxx::unexpected(EFrameExtensionRegistrationError::INVALID_NAME);
        }
        for (char c : name)
        {
            const bool is_valid_character = is_letter(c) || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!is_valid_character)
            {
                return lux::cxx::unexpected(EFrameExtensionRegistrationError::INVALID_NAME);
            }
        }
        const std::lock_guard lock{mutex_};
        if (const auto found = name_to_id_.find(name); found != name_to_id_.end())
        {
            return found->second;
        }
        if (name_to_id_.size() == kMaxFrameExtensionSlots)
        {
            return lux::cxx::unexpected(EFrameExtensionRegistrationError::CAPACITY);
        }
        const auto id = static_cast<FrameExtensionSlotId>(name_to_id_.size() + 1);
        name_to_id_.emplace(name, id);
        return id;
    }

    FrameExtensionSlotId FrameExtensionRegistry::idOf(std::string_view name) const noexcept
    {
        const std::lock_guard lock{mutex_};
        const auto found = name_to_id_.find(name);
        return found != name_to_id_.end() ? found->second : kInvalidExtSlot;
    }
} // namespace lux::render
