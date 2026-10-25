#pragma once

#include <lux/engine/description/LightDescription.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::render
{
    inline constexpr std::string_view kLitSceneCapability = "lux.render.capability.lit_scene";
    inline constexpr std::string_view kLitSceneConfigurationSchema = "lux.render.config.lit_scene";
    inline constexpr std::string_view kSceneGeometryInput = "lux.render.input.raster_scene_geometry";
    inline constexpr std::string_view kCameraInput = "lux.render.input.camera";

    // Versioned configuration for the built-in lit-scene implementations, in world units.
    // Applications persist these values, never a resolved CapabilitySelection.
    struct LitSceneConfiguration final
    {
        bool shadows = true;
        std::uint32_t shadow_resolution = 1024;
        std::uint32_t shadow_filter_radius = 1;
        float shadow_distance = 100;
        std::uint32_t cluster_index_capacity = 65536;
        bool operator==(const LitSceneConfiguration&) const noexcept = default;
    };

    // Portable configuration for project persistence, validated before encoding.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<SerializedRenderConfiguration>
    encodeLitSceneConfiguration(const LitSceneConfiguration&) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<
        LitSceneConfiguration> decodeLitSceneConfiguration(std::span<const std::byte>) noexcept;

    struct RasterGeometrySelection final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        bool operator==(const RasterGeometrySelection&) const noexcept = default;

        bool operator<(const RasterGeometrySelection& other) const noexcept
        {
            const auto key = [](const RasterGeometrySelection& value) noexcept
            { return std::tuple{value.mesh, value.material, value.submesh}; };
            return key(*this) < key(other);
        }
    };

    // Asset data needed for cold effect construction. Render selects the passes, bindings and draw commands.
    struct RasterGeometryInput final
    {
        MeshDescription geometry;
        rdesc::MaterialProgramSet program;
        RasterGeometrySelection selection;
        std::vector<ImageShape> textures;
        bool front_counter_clockwise = true;
        bool operator==(const RasterGeometryInput&) const noexcept = default;
    };

    struct SceneLightInput final
    {
        LightId light;
        rdesc::LightDescription description;
        bool operator==(const SceneLightInput&) const noexcept = default;
    };

    struct LitSceneInputs final
    {
        std::vector<RasterGeometryInput> geometry;
        std::vector<SceneLightInput> lights;
        std::uint32_t instance_capacity = 128;
        // Cold clipping distances used to select directional shadow cascades. Pose/FOV are frame inputs.
        float near_distance = 0.1F, far_distance = 1000;
    };

    // Column-major matrices; position/direction are world-space, direction is unit length.
    // This is semantic camera data, not a concatenation of implementation shader blocks.
    struct CameraInput final
    {
        TransformMatrix view = kIdentityTransform;
        TransformMatrix projection = kIdentityTransform;
        TransformMatrix inverse_view_projection = kIdentityTransform;
        std::array<float, 3> position{}, direction{0, 0, -1};
        float near_distance = 0.1F, far_distance = 1000;
        std::array<float, 4> viewport{0, 0, 1, 1};
        std::array<float, 4> background{};
        float exposure = 1;
        bool operator==(const CameraInput&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<LitSceneInputs> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const LitSceneInputs&) noexcept;
    };
} // namespace lux::render
