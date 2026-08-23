#pragma once

#if !defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
#error UISession diagnostics are available only to the benchmark test build
#endif

#include <cstdint>

#include <lux/engine/ui_next/UISession.hpp>

namespace lux::ui::detail
{
    struct UISessionDiagnosticsAccess final
    {
        [[nodiscard]] static std::uint64_t wrapperGrowthCount(
            const UISession& session
        ) noexcept
        {
            return session.wrapperGrowthCountForTest();
        }
    };
} // namespace lux::ui::detail
