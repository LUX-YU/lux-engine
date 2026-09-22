#pragma once

#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/detail/UiFontAtlas.hpp>

namespace lux::ui::detail
{
// Internal CPU integration only. Never transported to the renderer.
struct LUX_FUNCTION_PUBLIC ContextAccess final
{
    [[nodiscard]] static void *native(Context &) noexcept;
    [[nodiscard]] static UiFontAtlasResult fontAtlas(Context &);
};
} // namespace lux::ui::detail
