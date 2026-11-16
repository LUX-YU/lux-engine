#pragma once
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/scene/SceneSystem.hpp>

namespace lux::scene
{
    class LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC MeshQuerySystem final : public SceneSystem
    {
    public:
        inline static constexpr std::string_view Name = "lux.builtin.system.mesh_query.3d";

        explicit MeshQuerySystem(ecs::Registry& registry);
        ~MeshQuerySystem() noexcept;
        MeshQuerySystem(const MeshQuerySystem&) = delete;
        MeshQuerySystem& operator=(const MeshQuerySystem&) = delete;

        [[nodiscard]] Result execute() noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC SceneSystemRegistration
    builtinMeshQuerySystemRegistration() noexcept;
} // namespace lux::scene
