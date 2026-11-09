#pragma once

#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <string>

namespace lux::render::runtime_detail
{
    struct FeatureEntry final
    {
        // Stable table construction binds views only after their owned metadata exists.
        FeatureId id;
        std::string name;
        cxx::type_info config_type;
        FeatureGraphOps graph;

        FeatureEntry(FeatureName type, cxx::type_info config, const FeatureGraphOps& contribution) noexcept
            : name(type.name()), config_type(config), graph(contribution)
        {
        }
    };

} // namespace lux::render::runtime_detail
