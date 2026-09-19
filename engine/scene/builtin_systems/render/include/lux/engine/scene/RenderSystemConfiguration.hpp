#pragma once

#include <lux/engine/function/render/client/core/FeatureTypeId.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstddef>
#include <vector>

namespace lux::scene
{
    struct LUX_TYPE_INFO(both) RenderFeatureInstanceDescription final
    {
        LUX_MEMBER(readonly = true)
        render::FeatureTypeId type{};

        LUX_MEMBER(editor_widget = render_feature_config)
        std::vector<std::byte> configuration;
    };

    struct LUX_TYPE_INFO(both) RenderSystemConfiguration final
    {
        LUX_MEMBER(display_name = CoordinatePageSize, min = 1.0)
        double coordinate_page_size{1024.0};

        LUX_MEMBER(editor_widget = render_feature_list)
        std::vector<RenderFeatureInstanceDescription> features;
    };
} // namespace lux::scene
