#pragma once
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/EngineContext.hpp>

namespace lux::editor::material
{
    struct MaterialEditor::Impl::Preview final
    {
        explicit Preview(lux::scene::SceneRuntime& value) noexcept : runtime(value) {}
        ~Preview() noexcept
        {
            if (scene && !runtime.destroy(*scene))
                std::terminate();
        }
        lux::scene::SceneRuntime& runtime;
        std::optional<lux::scene::SceneInstanceId> scene;
        lux::simulation::ecs::Entity camera{lux::simulation::ecs::NullEntity}, sphere{lux::simulation::ecs::NullEntity};
        lux::scene::RenderSceneReceipt receipt;
        lux::asset::AssetId mesh;
        lux::cxx::SharedBytes<> mesh_image, world_volume;
        lux::scene::RenderAssetInput successful, candidate;
        editing::StateId displayed, pending;
        std::uint64_t generation{};
        std::string error;
    };
}
