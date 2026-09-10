#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/UiFontSource.hpp>
#include <lux/cxx/compile_time/expected.hpp>

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

    using UiFontAtlasResult = lux::cxx::expected<UiFontAtlasSnapshot, EUiInitError>;
    [[nodiscard]] LUX_FUNCTION_PUBLIC UiFontAtlasResult captureUiFontAtlas(UISession& session) noexcept;
} // namespace lux::ui::detail
