#pragma once

#include <lux/engine/editor/Toolset.hpp>

namespace lux::editor::detail
{
    struct ToolsetTestAccess final
    {
        [[nodiscard]] static lux::cxx::expected<void, ToolsetFailure> installOpaque(
            Toolset& toolset,
            lux::cxx::TypeToken type
        ) noexcept
        {
            auto* value = new (std::nothrow) int{};
            if (value == nullptr)
            {
                return lux::cxx::unexpected(ToolsetFailure{EToolsetError::ALLOCATION_FAILURE, type});
            }
            const auto destroy = [](void* object) noexcept { delete static_cast<int*>(object); };
            auto result = toolset.installErased(type, value, destroy, nullptr);
            if (!result)
            {
                destroy(value);
            }
            return result;
        }
    };
} // namespace lux::editor::detail
