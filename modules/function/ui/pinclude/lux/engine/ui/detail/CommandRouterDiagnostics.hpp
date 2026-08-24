#pragma once

#if !defined(LUX_UI_TEST_DIAGNOSTICS)
#error CommandRouter diagnostics are available only to the benchmark test build
#endif

#include <cstdint>
#include <span>

#include <lux/engine/ui/CommandRouter.hpp>

namespace lux::ui::detail
{
    struct CommandRouterDiagnosticsAccess final
    {
        static void updateRoute(CommandRouter &router, lux::object::LuxObject *activation_scope,
                                std::span<const UiContextIdView> contexts)
        {
            router.updateRoute(activation_scope, contexts);
        }

        [[nodiscard]] static std::span<const UiContextIdView> activeContexts(
            const CommandRouter &router) noexcept
        {
            return router.activeContextsForTest();
        }

        [[nodiscard]] static std::uint64_t rebuildCount(const CommandRouter &router) noexcept
        {
            return router.rebuildCountForTest();
        }

        [[nodiscard]] static std::uint64_t rebuildElapsedNanoseconds(
            const CommandRouter &router) noexcept
        {
            return router.rebuildElapsedForTest();
        }

        [[nodiscard]] static std::uint64_t storageGrowthCount(const CommandRouter &router) noexcept
        {
            return router.storageGrowthCountForTest();
        }
    };
} // namespace lux::ui::detail
