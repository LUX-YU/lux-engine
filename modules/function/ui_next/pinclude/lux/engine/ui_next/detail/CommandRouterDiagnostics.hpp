#pragma once

#include <cstdint>

#include <lux/engine/ui_next/CommandRouter.hpp>

namespace lux::ui::detail
{
    struct CommandRouterDiagnosticsAccess final
    {
        [[nodiscard]] static std::uint64_t rebuildCount(
            const CommandRouter& router
        ) noexcept
        {
            return router.rebuildCountForTest();
        }

        [[nodiscard]] static std::uint64_t rebuildElapsedNanoseconds(
            const CommandRouter& router
        ) noexcept
        {
            return router.rebuildElapsedForTest();
        }
    };
} // namespace lux::ui::detail
