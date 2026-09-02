#pragma once

#include <lux/engine/ui/ValueEdit.hpp>

#include <optional>
#include <string_view>

namespace lux::editor::inspector
{
    enum class EGeneratedWidget : unsigned char
    {
        DEFAULT,
        DRAG,
        SLIDER,
        INPUT,
        COLOR,
        ASSET,
        ENUM,
        READ_ONLY,
    };

    struct GeneratedFieldSpec final
    {
        std::string_view name;
        std::string_view display_name;
        std::string_view tooltip;
        EGeneratedWidget widget{EGeneratedWidget::DEFAULT};
        double speed{0.1};
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::optional<double> step;
        bool read_only{};
    };
} // namespace lux::editor::inspector
