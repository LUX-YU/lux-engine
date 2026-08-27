#pragma once

#include <cstdint>
#include <string>

#include <lux/engine/ui/UiIds.hpp>

namespace lux::ui
{
    class CommandHandle final
    {
    public:
        CommandHandle() noexcept = default;

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return owner_identity_ != 0;
        }
        [[nodiscard]] constexpr bool operator==(const CommandHandle&) const noexcept = default;

    private:
        friend class CommandRouter;
        constexpr CommandHandle(std::uint32_t dense_index, std::uint64_t owner_identity) noexcept
            : dense_index_(dense_index), owner_identity_(owner_identity)
        {
        }

        std::uint32_t dense_index_{0};
        std::uint64_t owner_identity_{0};
    };

    struct Command final
    {
        UiCommandId id;
        std::string label;
    };

    enum class ECommandDispatchResult
    {
        EXECUTED,
        DISABLED,
        NOT_FOUND
    };
} // namespace lux::ui
