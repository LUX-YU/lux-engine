#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Target.hpp>

#include <array>
#include <cstddef>
#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>
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

    [[nodiscard]] inline std::span<const std::byte> meshViewParameterBytes(const MeshViewParameters& values) noexcept
    {
        return std::as_bytes(std::span(&values, 1));
    }

    [[nodiscard]] inline rdesc::ShaderBlock meshViewParameterBlock() noexcept
    {
        rdesc::ShaderBlock result{sizeof(MeshViewParameters), 16};
        result.fields = {
            {"view", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"projection", rdesc::EDataScalar::FLOAT, 64, 4, 4, 0, 16}
        };
        return result;
    }

    struct MeshInstanceDrawGroup final
    {
        MeshId mesh;
        // Selects all instances using this material's cooked program table, not this one MaterialId.
        MaterialId material_table_source;
        std::uint32_t submesh = 0;
        bool operator==(const MeshInstanceDrawGroup&) const noexcept = default;

        bool operator<(const MeshInstanceDrawGroup& other) const noexcept
        {
            const auto key = [](const MeshInstanceDrawGroup& group) noexcept
            {
                return std::tuple{
                    group.mesh.storage.index,
                    group.mesh.storage.gen,
                    group.mesh.local.index,
                    group.mesh.local.gen,
                    group.material_table_source.storage.index,
                    group.material_table_source.storage.gen,
                    group.material_table_source.local.index,
                    group.material_table_source.local.gen,
                    group.submesh
                };
            };
            return key(*this) < key(other);
        }
    };

    // One geometry/material group. Feature chooses its rendering purpose and output attachments.
    struct RasterMeshGroup final
    {
        MeshDescription geometry;
        rdesc::MaterialProgramSet program;
        MeshInstanceDrawGroup group;
        std::string mesh_storage = "meshes";
        std::string material_storage = "materials";
        std::string instance_storage = "instances";
        std::vector<GraphImageDesc> textures;
        // Base geometry/projection winding before the instance transform. Negative determinants
        // are partitioned by the Feature and invert this state; callers must not compensate twice.
        bool front_counter_clockwise = true;
        bool operator==(const RasterMeshGroup&) const noexcept = default;
    };

    struct MeshFeatureConfig final
    {
        TargetDesc target;
        RasterMeshGroup mesh;
        bool operator==(const MeshFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<RasterMeshGroup> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const RasterMeshGroup&) noexcept;
    };

    template <> struct ConfigValueTraits<MeshFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const MeshFeatureConfig&) noexcept;
    };

} // namespace lux::render
