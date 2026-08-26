#pragma once

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/EcsState.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/world_section/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class WorldSectionTransaction;

    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionInstance final
    {
      public:
        WorldSectionInstance() noexcept = default;
        WorldSectionInstance(WorldSectionInstance&&) noexcept;
        WorldSectionInstance& operator=(WorldSectionInstance&&) noexcept = delete;
        ~WorldSectionInstance() noexcept;

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

        [[nodiscard]] bool active() const noexcept
        {
            return state_ == EState::ACTIVE;
        }

      private:
        enum class EState : std::uint8_t
        {
            INACTIVE,
            STAGED,
            ACTIVE,
        };

        WorldSectionId id_;
        std::vector<Entity> entities_;
        std::vector<std::shared_ptr<const void>> code_lifetimes_;
        std::uint64_t world_identity_{};
        std::uint64_t lease_{};
        EState state_{EState::INACTIVE};

        friend class WorldSectionTransaction;
    };

    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionTransaction final
    {
      public:
        WorldSectionTransaction(const WorldSectionTransaction&) = delete;
        WorldSectionTransaction& operator=(const WorldSectionTransaction&) =
            delete;
        WorldSectionTransaction(WorldSectionTransaction&&) noexcept;
        WorldSectionTransaction& operator=(WorldSectionTransaction&&) noexcept =
            delete;
        ~WorldSectionTransaction() noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldSectionFailure> load(
            const ComponentLoadSet& loads,
            const WorldSectionImage& image,
            WorldSectionInstance& inactive_output
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldSectionFailure> unload(
            WorldSectionInstance& instance
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldSectionFailure>
        commit() noexcept;

      private:
        struct Impl;
        explicit WorldSectionTransaction(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;

        friend LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC lux::cxx::expected<
            WorldSectionTransaction,
            WorldSectionFailure>
        beginWorldSectionTransaction(
            EcsState&,
            WorldSectionLoadScratchBudget,
            lux::serialization::SerializationLimits
        ) noexcept;
    };

    [[nodiscard]] LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC lux::cxx::expected<
        WorldSectionTransaction,
        WorldSectionFailure>
    beginWorldSectionTransaction(
        EcsState& world,
        WorldSectionLoadScratchBudget scratch,
        lux::serialization::SerializationLimits limits
    ) noexcept;
}
