#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
namespace lux::editor::material
{
    struct MaterialPreviewEnvironment final
    {
        scene::ProjectionEnvironment scene;
        std::vector<render::RenderFeatureRegistration> features;
    };
    struct MaterialPreviewStatus final
    {
        MaterialCompileKey desired;
        std::optional<MaterialCompileKey> prepared, accepted;
        bool stale{};
        std::string diagnostic;
    };
    // One preview target. Other targets use independent stores, sharing Runtime and RenderResources.
    class MaterialPreviewStore final
    {
    public:
        MaterialPreviewStore(lux::scene::SceneRuntime&, MaterialPreviewEnvironment);
        ~MaterialPreviewStore();
        [[nodiscard]] std::uint64_t target() const noexcept;
        void setDesired(MaterialCompileKey) noexcept;
        // Terminal CPU facts may arrive during a caller's dispatch; adoption occurs only in update.
        [[nodiscard]] MaterialCompileResult<void> receive(
            const MaterialCompileOperation&,
            lux::scene::RenderAssetInput
        );
        void update() noexcept;
        [[nodiscard]] MaterialCompileResult<void> reset(lux::scene::RenderAssetInput);
        [[nodiscard]] MaterialCompileResult<void>
        navigate(const simulation::ecs::Transform3D&, const lux::scene::Camera&);
        [[nodiscard]] MaterialPreviewStatus status() const;
        [[nodiscard]] simulation::ecs::Entity camera() const noexcept;
        [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] lux::scene::InstanceRetirement close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
