#pragma once

#if defined(LUX_EDITOR_UI_DIAGNOSTICS)
#include <cstdio>

namespace lux::editor::ui
{
    struct UiMeasurement final
    {
        const char* pane;
        std::size_t objects{}, rows{}, binding_queries{}, directory_rebuilds{};
        ~UiMeasurement()
        {
            std::printf(
                "DIAGNOSTIC pane=%s objects=%zu rows=%zu binding_queries=%zu directory_rebuilds=%zu\n",
                pane,
                objects,
                rows,
                binding_queries,
                directory_rebuilds
            );
        }
    };
} // namespace lux::editor::ui
#define LUX_UI_MEASURE(statement) statement
#else
#define LUX_UI_MEASURE(statement) ((void)0)
#endif
