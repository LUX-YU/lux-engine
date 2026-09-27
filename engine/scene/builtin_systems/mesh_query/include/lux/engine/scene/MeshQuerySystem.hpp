#pragma once
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>

namespace lux::scene
{
    class LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC MeshQuerySystem final
    {
    public:
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.mesh_query.3d"}};
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.builtin.system.mesh_query.3d",
            .version = 1U,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .supported_world_types = SupportedWorldTypes
        };

        explicit MeshQuerySystem(simulation::ecs::Registry& registry);
        ~MeshQuerySystem() noexcept;
        MeshQuerySystem(const MeshQuerySystem&) = delete;
        MeshQuerySystem& operator=(const MeshQuerySystem&) = delete;

        void updateStablePoint();
        [[nodiscard]] bool hasPendingChanges() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC SceneSystemRegistration
    builtinMeshQuerySystemRegistration() noexcept;
} // namespace lux::scene
