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
        [[nodiscard]] static UiDrawDataSnapshot capture(UISession& session);
        [[nodiscard]] static UiFontAtlasSnapshot captureFontAtlas(UISession& session);
    };
} // namespace lux::ui::detail
