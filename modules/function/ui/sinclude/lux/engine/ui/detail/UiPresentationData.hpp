#pragma once

#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui
{
    class UISession;
}

namespace lux::ui::detail
{
    struct UISessionPresentationAccess;

    struct UiFontAtlasSnapshot final
    {
        std::vector<std::uint8_t> pixels;
        int width{};
        int height{};
        void* context{};
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC UiFontAtlasSnapshot captureUiFontAtlas(UISession& session);
} // namespace lux::ui::detail
