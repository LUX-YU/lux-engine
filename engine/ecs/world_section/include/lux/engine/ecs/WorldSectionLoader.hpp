#pragma once

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/world_section/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionInstance final
    {
      public:
        WorldSectionInstance() noexcept = default;
        WorldSectionInstance(WorldSectionInstance&&) noexcept = default;
        WorldSectionInstance& operator=(WorldSectionInstance&&) noexcept = default;
        ~WorldSectionInstance() = default;

        WorldSectionInstance(const WorldSectionInstance&) = delete;
        WorldSectionInstance& operator=(const WorldSectionInstance&) = delete;

        [[nodiscard]] const WorldSectionId& id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] std::span<const Entity> entities() const noexcept
        {
            return entities_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return entities_.empty();
        }

      private:
        WorldSectionId id_;
        std::vector<Entity> entities_;

        friend class WorldSectionLoader;
    };

    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionLoader final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<
            WorldSectionInstance,
            WorldSectionFailure>
        load(
            World& world,
            const ComponentLoadSet& loads,
            const WorldSectionImage& image,
            lux::serialization::SerializationLimits limits = {}
        ) noexcept;

        [[nodiscard]] static lux::cxx::expected<
            void,
            WorldSectionFailure>
        unload(
            World& world,
            WorldSectionInstance& instance
        ) noexcept;
    };
} // namespace lux::ecs
