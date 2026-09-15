#pragma once

#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>

#if defined(_WIN32)
#if defined(CONSUMER_GUI_LIBRARY)
#define CONSUMER_GUI_PUBLIC __declspec(dllexport)
#else
#define CONSUMER_GUI_PUBLIC __declspec(dllimport)
#endif
#else
#define CONSUMER_GUI_PUBLIC
#endif

namespace consumer
{
    [[nodiscard]] CONSUMER_GUI_PUBLIC lux::editor::gui::ComponentBinding binding();
    [[nodiscard]] CONSUMER_GUI_PUBLIC std::size_t drawCount() noexcept;

    struct DrawSample final
    {
        std::size_t warmup{}, draws{};
        double active_microseconds{};
    };
    CONSUMER_GUI_PUBLIC void beginDrawSample() noexcept;
    [[nodiscard]] CONSUMER_GUI_PUBLIC DrawSample drawSample() noexcept;
} // namespace consumer
