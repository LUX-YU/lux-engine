#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/editor/ui/render/Ui.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>

namespace lux::editor::ui
{
    using namespace lux::render;

    struct UiRasterFeatureConfig final
    {
        EffectOutputDesc output;
        UiConfiguration draw;
        bool operator==(const UiRasterFeatureConfig&) const noexcept = default;
    };

    struct UiRasterFeature final
    {
        using Config = UiRasterFeatureConfig;
        static constexpr FeatureName name{"lux.editor.ui.raster"};

        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };
} // namespace lux::editor::ui
