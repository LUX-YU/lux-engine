#pragma once

#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <Eigen/Geometry>

#include <map>
#include <unordered_map>

namespace lux::scene::detail
{
    using namespace lux::render;
    using engine::render::RenderAssetVersion;

    struct MaterialEntry final
    {
        MaterialId material;
        std::vector<RenderAssetVersion> textures;
        std::size_t references = 0;
        std::uint64_t program_hash = 0;
        asset::AssetVfsView snapshot;
        RenderError error;
    };

    struct MeshBinding final
    {
        rdesc::MeshVisualDescription visual;
        std::uint64_t revision = 0;
    };

    struct MeshEntry final
    {
        MeshBinding binding;
        // Candidate uploads never invalidate the currently drawn binding. Previous is retained
        // only until every observation has stopped referring to its compiled resource groups.
        std::optional<MeshBinding> replacement, previous;
        MeshInstanceId instance;
        RenderError error;
        bool present = true;
    };

    struct InstanceInput final
    {
        std::uint64_t entity;
        MeshInstanceDescription description;
    };

    struct LightEntry final
    {
        LightId light;
        rdesc::ELightType type = rdesc::ELightType::POINT;
        bool present = true;
    };

    struct LightInput final
    {
        std::uint64_t entity;
        VDirectLight value;
    };

    using SceneMaterials = std::map<RenderAssetVersion, MaterialEntry>;
    using SceneMeshes = std::unordered_map<std::uint64_t, MeshEntry>;
    using SceneLights = std::unordered_map<std::uint64_t, LightEntry>;

    // Owned cold data, rebuilt only after geometry/light structure changes. No Scene or GPU owner.
    struct SceneEffectInputs final
    {
        std::vector<RasterGeometryInput> geometry;
        std::vector<SceneLightInput> lights;
        std::uint32_t capacity = 0;
    };

    using EffectInputProducer = RenderResult<ConfigValue> (*)(const SceneEffectInputs&, const CameraInput&) noexcept;

    [[nodiscard]] std::uint64_t programHash(const rdesc::MaterialProgramSet&) noexcept;
    [[nodiscard]] RenderResult<VDirectLight>
    lightValue(const rdesc::LightDescription&, const Eigen::Affine3d&) noexcept;
    [[nodiscard]] bool bindingReady(
        const MeshBinding&,
        const engine::render::RenderAssets&,
        const SceneMaterials&,
        std::uint64_t source,
        RenderError& failure
    ) noexcept;
    [[nodiscard]] SceneEffectInputs prepareSceneEffectInputs(
        const SceneMeshes&,
        const SceneMaterials&,
        const SceneLights&,
        const simulation::ecs::Registry&,
        const engine::render::RenderAssets&,
        std::uint64_t source,
        std::uint32_t previous_capacity,
        bool include_replacements
    ) noexcept;
    [[nodiscard]] RenderResult<ConfigValue>
    produceLitSceneInputs(const SceneEffectInputs&, const CameraInput&) noexcept;
} // namespace lux::scene::detail
