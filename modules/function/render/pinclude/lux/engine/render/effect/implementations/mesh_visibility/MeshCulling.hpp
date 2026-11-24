#pragma once

#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/description/MaterialProgramSet.hpp>

#include <lux/engine/render/effect/EffectImplementation.hpp>

namespace lux::render::detail
{
    struct ShadowProjectionSettings;

    struct MeshRasterAttachments final
    {
        rdesc::EMaterialPurpose purpose;
        bool counter_clockwise;
        RasterWork work;
    };

    struct ShadowVisibilityPass final
    {
        PassResult raster;
        std::array<GraphResourceId, 3> shadow_buffers;
    };

    [[nodiscard]] RenderResult<bool> parseFrustumCulling(graph::NodeId, std::string_view) noexcept;

    template <RenderResult<void> (*Build)(EffectCompileContext&, bool) noexcept>
    class CameraVisibility final : public EffectImplementation
    {
    public:
        explicit CameraVisibility(bool cull) noexcept : EffectImplementation(sizeof(cull)), cull_(cull) {}

        static EffectCreateResult create(graph::NodeId node, std::string_view arguments) noexcept
        {
            auto cull = parseFrustumCulling(node, arguments);
            if (!cull) return cxx::unexpected(cull.error());
            return std::make_unique<CameraVisibility>(*cull);
        }

        RenderResult<void> onBuild(EffectCompileContext& context) const noexcept override { return Build(context, cull_); }

    private:
        bool cull_;
    };

    [[nodiscard]] RenderResult<PassResult>
    appendCameraVisibility(EffectCompileContext&, std::optional<MeshRasterAttachments>, bool frustum_culling) noexcept;
    [[nodiscard]] RenderResult<ShadowVisibilityPass> appendShadowVisibility(
        EffectCompileContext&, std::optional<MeshRasterAttachments>, const ShadowProjectionSettings&, bool frustum_culling
    ) noexcept;

    struct MeshVisibilityBuffers final
    {
        ResourceVersion transforms, alive, properties, views;
        ResourceVersion groups, candidates, chunks, visible, commands, dispatch;
    };

    struct MeshCullConstants final { std::uint32_t frustum_culling, shadow_pass; };

    struct MeshVisibilityOutputs final
    {
        ResourceVersion visible, commands;
    };

    [[nodiscard]] RenderResult<MeshVisibilityOutputs> addMeshVisibilityPasses(
        EffectCompileContext&, const std::string& pass_prefix, const MeshVisibilityBuffers&,
        MeshCullConstants
    ) noexcept;
} // namespace lux::render::detail
