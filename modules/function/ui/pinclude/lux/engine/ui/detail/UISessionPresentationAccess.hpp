#pragma once

#include <lux/engine/ui/detail/UiPresentationData.hpp>

#include <cstdint>
#include <vector>

namespace lux::ui
{
    class UISession;
}

namespace lux::ui::detail
{
    struct UISessionPresentationAccess final
    {
        [[nodiscard]] static UiFontAtlasResult captureFontAtlas(UISession& session) noexcept;
    };
} // namespace lux::ui::detail
