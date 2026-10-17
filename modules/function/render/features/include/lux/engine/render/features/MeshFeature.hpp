#pragma once

#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/resource/Instance.hpp>

namespace lux::render
{
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
        InstanceBinding instances;
        std::string mesh_storage = "meshes";
        std::string material_storage = "materials";
        std::string instance_storage = "instances";
        std::vector<GraphImageDesc> textures;
        bool readback = false; // Explicit color/depth Copy outputs for consumers that request them.
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
}
