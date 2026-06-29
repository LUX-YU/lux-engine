#pragma once
// ============================================================================
//  GridOperation.hpp — Grid-pass feature command payloads + operation IDs
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Query name constants — use with queryTypeId() / findTypeId()
    // =========================================================================
    // =========================================================================
    //  Default shader name constants for GridPassFeature
    // =========================================================================
    inline constexpr std::string_view kGridVertShaderName = "grid.vert";
    inline constexpr std::string_view kGridFragShaderName = "grid.frag";

    /// Comm-layer config for GridPassFeature.
    /// The client fills this with shader indices returned by CompileShader.
    /// The factory create_fn resolves these to real ShaderHandle on the server.
    struct GridCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
    };
    static_assert(std::is_trivially_copyable_v<GridCommConfig>);

    struct GridSetParamsPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float planeY{0.f};
        float cellSize{1.f};
        float linePx{1.1f};
        float fadeDist{50.f};
        float holeRatio{0.45f};
    };
    static_assert(std::is_trivially_copyable_v<GridSetParamsPayload>);

    /// Typed op declaration — the single source the client Ids and the server
    /// registrar both derive from. Stream op (fire-and-forget, no reply); the
    /// handler lives in GridOperationHandlers.cpp.
    struct GridSetParamsOp
    {
        using Payload = GridSetParamsPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "GridSetParams";
    };

    /// Operation IDs returned to the client after RegisterFeatureType.
    using GridOperationIds = FeatureOpIds<GridSetParamsOp>;

    extern LUX_FUNCTION_PUBLIC const FeatureFactory kGridFeatureFactory;

    // =========================================================================
    //  Client-side proxy — GridProxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC GridProxy
    {
    public:
        GridProxy(RenderSession& session, GridOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Update grid visual parameters at runtime.
        void setParams(
            RenderSceneId scene_id, FeatureHandle feature,
            float planeY = 0.f, float cellSize = 1.f,
            float linePx = 1.1f, float fadeDist = 50.f,
            float holeRatio = 0.45f
        );

    private:
        RenderSession*   session_;
        GridOperationIds ops_;
    };
} // namespace lux::render
