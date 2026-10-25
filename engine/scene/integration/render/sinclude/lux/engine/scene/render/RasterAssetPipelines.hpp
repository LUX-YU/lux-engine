#pragma once

#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/render/inputs/ImageShape.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

namespace lux::scene
{
    struct RenderMesh final
    {
        render::MeshId mesh;
        render::MeshDescription geometry;
    };

    struct RenderTexture final
    {
        render::TextureId texture;
        render::ImageShape description;
    };

    struct MeshAssetPipeline final
    {
        using Value = RenderMesh;
        static constexpr std::string_view name = "lux.scene.asset.indexed_mesh";
        engine::render::RenderExecution& execution;
        render::CapabilityInput input;
        engine::render::BoundCommand<render::CreateMesh> create;
        engine::render::BoundCommand<render::ReleaseMesh> remove;

        exec::task<render::RenderResult<Value>> load(engine::render::RenderAssetLoad) noexcept;
        exec::task<render::RenderResult<void>> release(Value&) noexcept;
    };

    struct TextureAssetPipeline final
    {
        using Value = RenderTexture;
        static constexpr std::string_view name = "lux.scene.asset.sampled_texture";
        engine::render::RenderExecution& execution;
        render::CapabilityInput input;
        engine::render::BoundCommand<render::CreateTexture> create;
        engine::render::BoundCommand<render::ReleaseTexture> remove;

        exec::task<render::RenderResult<Value>> load(engine::render::RenderAssetLoad) noexcept;
        exec::task<render::RenderResult<void>> release(Value&) noexcept;
    };

    struct MaterialAssetPipeline final
    {
        using Value = std::shared_ptr<const asset::MaterialAsset>;
        static constexpr std::string_view name = "lux.scene.asset.cooked_material";
        execution::CpuScheduler cpu;

        exec::task<render::RenderResult<Value>> load(engine::render::RenderAssetLoad) noexcept;
        exec::task<render::RenderResult<void>> release(Value&) noexcept;
    };

    [[nodiscard]] exec::task<render::RenderResult<void>> installRasterAssetPipelines(
        engine::render::RenderAssets&,
        render::CapabilityQuery,
        render::VCapabilityConfiguration = {}
    ) noexcept;

    [[nodiscard]] const rdesc::MaterialDescription* cookedMaterial(
        const engine::render::RenderAssets&,
        engine::render::RenderAssetVersion
    ) noexcept;
} // namespace lux::scene
