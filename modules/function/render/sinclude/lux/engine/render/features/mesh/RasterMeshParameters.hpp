#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Target.hpp>

#include <array>
#include <cstddef>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <span>
#include <type_traits>

namespace lux::render
{
    // Explicit Mesh shader inputs. The caller computes these column-major matrices.
    struct MeshViewParameters final
    {
        std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::array<float, 16> projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    static_assert(
        std::is_standard_layout_v<MeshViewParameters> && sizeof(MeshViewParameters) == 128 &&
        offsetof(MeshViewParameters, projection) == 64
    );

    struct MeshInstanceDrawGroup final
    {
        MeshId mesh;
        // Selects all instances using this material's cooked program table, not this one MaterialId.
        MaterialId material_source;
        std::uint32_t submesh = 0;
        bool operator==(const MeshInstanceDrawGroup&) const noexcept = default;

        bool operator<(const MeshInstanceDrawGroup& other) const noexcept
        {
            const auto key = [](const MeshInstanceDrawGroup& group) noexcept
            { return std::tuple{group.mesh, group.material_source, group.submesh}; };
            return key(*this) < key(other);
        }
    };

    struct MeshFeatureConfig final
    {
        EffectOutputDesc output;
        bool front_counter_clockwise = true;
        bool operator==(const MeshFeatureConfig&) const noexcept = default;
    };

} // namespace lux::render
