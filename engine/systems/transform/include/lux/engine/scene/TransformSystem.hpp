#pragma once

#include <lux/engine/scene/TransformSystemConfiguration.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/transform/visibility.h>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>

#include <string_view>
#include <memory>
#include <vector>

namespace lux::scene
{
    struct SceneSystemRegistration;

    enum class ETransformUpdateError : std::uint8_t
    {
        INVALID_HIERARCHY,
        CAPACITY_EXCEEDED,
        COMMAND_RECORDING_FAILED,
        CONFIGURATION_ENCODE_FAILURE,
    };

    class LUX_ENGINE_SCENE_TRANSFORM_PUBLIC TransformSystem final : public SceneSystem
    {
    public:
        inline static constexpr std::string_view Name = "lux.scene.transform";

        explicit TransformSystem(ecs::Registry& registry);
        ~TransformSystem() noexcept;
        TransformSystem(const TransformSystem&) = delete;
        TransformSystem& operator=(const TransformSystem&) = delete;

        [[nodiscard]] Result prepare(const TransformSystemConfiguration& configuration) noexcept;
        [[nodiscard]] Result execute() noexcept override;
        [[nodiscard]] ecs::EcsCommandProducerCapacity commandCapacity() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_TRANSFORM_PUBLIC SceneSystemRegistration transformSystemRegistration() noexcept;
    using TransformConfigurationResult = lux::cxx::expected<std::vector<std::byte>, ETransformUpdateError>;
    [[nodiscard]] LUX_ENGINE_SCENE_TRANSFORM_PUBLIC TransformConfigurationResult makeTransformSystemConfiguration(
        std::size_t entity_capacity,
        ecs::EcsCommandProducerCapacity command_capacity
    ) noexcept;
} // namespace lux::scene
