#pragma once

#include <array>
#include <cstddef>
#include <lux/engine/render/data/ShaderBlock.hpp>
#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/resource/MeshInstance.hpp>
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

    [[nodiscard]] inline ShaderBlock meshViewParameterBlock() noexcept
    {
        ShaderBlock result{sizeof(MeshViewParameters), 16};
        result.fields = {
            {"view", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"projection", rdesc::EDataScalar::FLOAT, 64, 4, 4, 0, 16}
        };
        return result;
    }

    // One cold Draw selection. Assets are Global; instance records belong to the attached Scene.
    // Storage keys select existing owner-local instances; qualified resource IDs prove their identity.
    // This first Unlit Feature targets RGBA8 UNORM/SRGB and D32 depth; Native retains its wider format API.
    // BasicMesh depth shares the color attachment extent. DepthOnly is an independent fixed-resolution
    // depth producer (config.target extent); its required color Target is separately cleared. Reconfigure
    // the Effect to change that depth extent. Resizing only the color Target does not resize its depth map.
    struct MeshFeatureConfig final
    {
        TargetDesc target;
        MeshBinding mesh;
        MaterialBinding material;
        MeshInstanceBinding instances;
        std::string mesh_storage = "meshes";
        std::string material_storage = "materials";
        std::string instance_storage = "instances";
        std::vector<GraphImageDesc> textures;
        bool operator==(const MeshFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshFeatureConfig> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MeshFeatureConfig&) noexcept;
    };

    struct BasicMeshFeature final
    {
        using Config = MeshFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.BasicMeshFeature"};
        static const FeatureDescriptor descriptor;
        static const FeatureGraphOps graph;
    };

    struct DepthOnlyFeature final
    {
        using Config = MeshFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.DepthOnlyFeature"};
        static const FeatureDescriptor descriptor;
        static const FeatureGraphOps graph;
    };
} // namespace lux::render
