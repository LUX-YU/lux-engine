#pragma once
/**
 * @file EntitySectionService.hpp
 * @brief Bounded, typed LXES loading operation for EntitySection owners.
 */

#include <lux/engine/ecs/entity_scene/EntitySectionLoadPort.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::runtime::entity_scene
{
    /// Process-level operation registration. Scene residency and ECS
    /// publication are intentionally owned by EntitySectionLoaderSystem.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySectionService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            EntitySectionService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            std::shared_ptr<const EntitySectionGeneratorCatalog> generators =
                {});

        EntitySectionService(const EntitySectionService&) = delete;
        EntitySectionService& operator=(const EntitySectionService&) = delete;
        EntitySectionService(EntitySectionService&& other) noexcept;
        EntitySectionService& operator=(EntitySectionService&& other) noexcept;
        ~EntitySectionService();

        [[nodiscard]] lux::ecs::entity_scene::EntitySectionLoadPort
        loadClient() const noexcept;
        void close() noexcept;

    private:
        EntitySectionService(
            std::shared_ptr<
                lux::ecs::entity_scene::detail::EntitySectionLoadState>
                control,
            lux::async::OperationPort<
                lux::ecs::entity_scene::LoadEntitySection> operation)
            noexcept;

        std::shared_ptr<
            lux::ecs::entity_scene::detail::EntitySectionLoadState> control_;
        lux::async::OperationPort<
            lux::ecs::entity_scene::LoadEntitySection> operation_;
        bool closed_{false};
    };
}
