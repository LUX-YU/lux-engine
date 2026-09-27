#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/transform/visibility.h>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

#include <array>
#include <memory>
#include <vector>

namespace lux::scene
{
    enum class ETransformUpdateError : std::uint8_t
    {
        INVALID_HIERARCHY,
        CAPACITY_EXCEEDED,
        COMMAND_RECORDING_FAILED,
        CONFIGURATION_ENCODE_FAILURE,
    };

    struct LUX_TYPE_INFO(both) TransformSystemConfiguration final
    {
        LUX_MEMBER(min = 1) std::uint64_t entity_capacity {};
        LUX_MEMBER(min = 1) std::uint64_t max_commands {};
        LUX_MEMBER(min = 1) std::uint64_t max_payload_bytes {};
    };

    class LUX_ENGINE_SCENE_TRANSFORM_PUBLIC TransformSystem final
    {
    public:
        inline static constexpr std::array Capabilities{
            std::string_view{"transform.2d"},
            std::string_view{"transform.3d"}
        };
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.transform",
            .version = 1,
            .configuration_schema_name = "lux.scene.transform.Configuration",
            .configuration_schema_version = 1,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .supported_world_types = SupportedWorldTypes
        };

        explicit TransformSystem(simulation::ecs::Registry& registry);
        ~TransformSystem() noexcept;
        TransformSystem(const TransformSystem&) = delete;
        TransformSystem& operator=(const TransformSystem&) = delete;

        [[nodiscard]] SceneStageResult prepare(const TransformSystemConfiguration& configuration) noexcept;
        [[nodiscard]] SceneStageResult synchronize(SceneStageContext& context) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_TRANSFORM_PUBLIC SceneSystemRegistration transformSystemRegistration() noexcept;
    using TransformConfigurationResult = lux::cxx::expected<std::vector<std::byte>, ETransformUpdateError>;
    [[nodiscard]] LUX_ENGINE_SCENE_TRANSFORM_PUBLIC TransformConfigurationResult makeTransformSystemConfiguration(
        std::size_t entity_capacity,
        simulation::ecs::EcsCommandProducerCapacity command_capacity
    ) noexcept;
}
