#pragma once

#include <lux/engine/editor/scene/SceneConfigurationDraft.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <variant>

namespace lux::simulation { class SimulationSystemRegistry; }
namespace lux::scene { struct SceneSystemRegistration; struct RenderFeatureSceneBinding; }
namespace lux::render { struct RenderFeatureRegistration; }

namespace lux::editor::scene
{
    enum class EScenePreparationError : std::uint8_t { INVALID_ARGUMENT, MISSING_PROVIDER, NOT_APPLICABLE };
    struct ScenePreparationFailure final
    {
        using VCause = std::variant<std::monostate, serialization::SerializationFailure,
            simulation::SimulationDescriptionFailure, lux::scene::SceneDescriptionFailure, asset::AssetDecodeFailure>;
        EScenePreparationError code{};
        std::string domain;
        std::uint64_t reason{};
        std::string message;
        VCause cause;
    };
    template<class T> using ScenePreparationResult = cxx::expected<T, ScenePreparationFailure>;

    // These inputs are borrowed only for this synchronous call; drafts retain no metadata pointers.
    struct SceneProviderOption final
    {
        std::string_view capability, name;
    };
    struct SceneConfigurationRegistrations final
    {
        const simulation::ecs::ComponentSchemaSet& components;
        const simulation::SimulationSystemRegistry& simulation_systems;
        std::span<const lux::scene::SceneSystemRegistration> scene_systems;
        std::span<const render::RenderFeatureRegistration> features;
        std::span<const SceneProviderOption> providers;
        std::span<const lux::scene::RenderFeatureSceneBinding> feature_bindings;
    };

    [[nodiscard]] ScenePreparationResult<SceneCreationConfiguration> prepareSceneConfiguration(
        const SceneConfigurationDraft&, const SceneConfigurationRegistrations&
    );
    [[nodiscard]] ScenePreparationResult<SceneConfiguration> prepareSceneConfigurationEdit(
        const SceneConfigurationDraft&, const SceneConfigurationRegistrations&
    );
    [[nodiscard]] ScenePreparationResult<SceneConfigurationDraft> makeSceneConfigurationPreset(
        ESceneContentPreset, std::string partition, std::uint32_t partition_version,
        const SceneConfigurationRegistrations&
    );
}
