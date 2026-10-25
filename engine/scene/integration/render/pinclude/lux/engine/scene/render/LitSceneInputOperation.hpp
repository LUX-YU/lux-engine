#pragma once
#include <lux/engine/scene/render/LightRenderResourceComponent.hpp>
#include <lux/engine/scene/render/MeshRenderBinding.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>

namespace lux::scene
{
    class LitSceneInputOperation final
    {
    public:
        using SourceComponents = std::tuple<>;
        using ResultComponents = std::tuple<>;
        using AggregateDependencies = std::tuple<MeshRenderBinding>;
        static constexpr std::string_view name = "lux.scene.operation.lit_scene_inputs";
        static constexpr auto policy = ERenderUpdatePolicy::STRUCTURE;
        static constexpr auto scope = ERenderOperationScope::SCENE;
        static std::span<const render::RenderPayloadContract> inputs() noexcept;
        static render::RenderResult<std::unique_ptr<LitSceneInputOperation>> create(RenderBindingContext&) noexcept;
        std::uint64_t effectRevision() noexcept;
        render::RenderResult<render::ConfigValue> effectInputs(cxx::TypeToken, std::span<const std::byte>) noexcept;

    private:
        LitSceneInputOperation(RenderBindingContext& context, std::vector<MeshRenderBinding*> meshes) noexcept
            : registry_(context.registry), meshes_(std::move(meshes)), revision_(context.input_revision)
        {
        }

        simulation::ecs::Registry& registry_;
        std::vector<MeshRenderBinding*> meshes_;
        const std::uint64_t& revision_;
        std::uint64_t cached_revision_ = 0, effect_revision_ = 1;
        render::LitSceneInputs inputs_;
    };
} // namespace lux::scene
