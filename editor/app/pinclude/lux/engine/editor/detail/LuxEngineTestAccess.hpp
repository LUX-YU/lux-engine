#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
namespace lux::editor
{
    class LuxEngine;
    namespace detail
    {
        // Source qualification only. Never installed or used by SDK consumers.
        struct LuxEngineTestAccess final
        {
            [[nodiscard]] static FrameworkResult<bool> pumpOnce(LuxEngine&) noexcept;
        };
    } // namespace detail
} // namespace lux::editor
