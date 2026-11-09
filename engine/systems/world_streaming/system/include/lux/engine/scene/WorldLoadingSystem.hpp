#pragma once

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/world_loading/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <cstddef>
#include <optional>
#include <vector>

namespace lux::ecs { class ComponentSchemaSet; }
namespace lux::world { class World; }
namespace lux::spatial { struct Spatial3DQueryBounds; }

namespace lux::engine
{
    class EngineContext;
}

namespace lux::scene
{
    struct WorldLoadingLimits final
    {
        std::size_t in_flight{4};
        std::size_t groups{64};
        std::size_t entities{100000};
        std::size_t component_bytes{64 * 1024 * 1024};
        // Maximum encoded block and per-block decoded working budget; not the full group peak.
        std::size_t max_block_bytes{16 * 1024 * 1024};
        // Sum of accepted encoded ranges; decoded values and prepared ECS data are additional.
        std::size_t max_in_flight_encoded_bytes{64 * 1024 * 1024};
    };

    // A Scene-owned consumer of compiled load groups. Construction does not start borrowed work.
    class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC WorldLoadingSystem final : public SceneSystem
    {
    public:
        // External assembly assigns one author-content loader per geometry (including the null geometry).
        // This System does not inspect sibling Systems; adoption still rejects duplicate object UUIDs.
        [[nodiscard]] static cxx::expected<std::unique_ptr<WorldLoadingSystem>, error::Error>
        create(
            engine::EngineContext& context,
            world::World& world,
            const ecs::ComponentSchemaSet& schemas,
            asset::AssetId source,
            std::optional<std::uint32_t> geometry,
            bool load_all,
            WorldLoadingLimits limits
        ) noexcept;
        ~WorldLoadingSystem() noexcept;
        [[nodiscard]] ESceneSystemStage stage() const noexcept override;
        [[nodiscard]] cxx::expected<void, error::Error> execute() noexcept override;
        [[nodiscard]] cxx::expected<void, error::Error> retry(std::uint32_t group) noexcept;
        [[nodiscard]] cxx::expected<void, error::Error> status() const noexcept;
        [[nodiscard]] cxx::expected<void, error::Error>
        queryGroups(const spatial::Spatial3DQueryBounds& bounds, std::vector<std::uint32_t>& groups) const noexcept;

    protected:
        [[nodiscard]] exec::task<Result> initialize() noexcept override;
        [[nodiscard]] exec::task<Result> releaseResources() noexcept override;

    private:
        struct Impl;
        explicit WorldLoadingSystem(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] bool refreshDemands() noexcept;
        void startNeededGroups() noexcept;
        [[nodiscard]] exec::task<cxx::expected<void, error::Error>> loadGroup(std::uint32_t group) noexcept;
        void releaseUnusedGroups() noexcept;
        [[nodiscard]] Result adoptGroup(std::uint32_t group) noexcept;
        [[nodiscard]] bool canUnload(std::uint32_t group) const noexcept;
        void unloadGroup(std::uint32_t group) noexcept;
        void reportGroupFailure(std::uint32_t group, error::Error failure) noexcept;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::scene
