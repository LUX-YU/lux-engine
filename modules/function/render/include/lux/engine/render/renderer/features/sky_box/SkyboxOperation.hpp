#pragma once
// ============================================================================
//  SkyboxOperation.hpp — Skybox feature command payloads + operation IDs
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderResourceHandle.hpp>
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
    //  Default shader name constants for SkyboxFeature
    // =========================================================================
    inline constexpr std::string_view kSkyboxVertShaderName         = "skybox.vert";
    inline constexpr std::string_view kSkyboxCubemapFragShaderName  = "skybox_cubemap.frag";
    inline constexpr std::string_view kSkyboxEquirectFragShaderName = "skybox_equirect.frag";

    /// Comm-layer config for SkyboxFeature.
    struct SkyboxCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle cubemap_fragment{};
        ShaderHandle equirect_fragment{};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxCommConfig>);

    struct SkyboxSetEquirectPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        RTextureHandle texture{};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxSetEquirectPayload>);

    struct SkyboxSetCubemapPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        RTextureHandle cube{};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxSetCubemapPayload>);

    // Typed ops — both fire-and-forget (CommandOp, no reply).
    struct SkyboxSetEquirectOp
    {
        using Payload = SkyboxSetEquirectPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "SkyboxSetEquirect";
    };
    struct SkyboxSetCubemapOp
    {
        using Payload = SkyboxSetCubemapPayload;
        static constexpr EOpKind kind = EOpKind::Stream;
        static constexpr const char* name = "SkyboxSetCubemap";
    };

    /// Operation IDs returned to the client after RegisterFeatureType. A real
    /// (forward-declarable) struct rather than a `using` alias, because gameplay
    /// headers take it by value with only a forward declaration.
    struct SkyboxOperationIds
        : FeatureOpIds<SkyboxSetEquirectOp, SkyboxSetCubemapOp>
    {
        using Ids = FeatureOpIds<SkyboxSetEquirectOp, SkyboxSetCubemapOp>;
        SkyboxOperationIds() = default;
        SkyboxOperationIds(const Ids& base) noexcept : Ids(base) {}
        [[nodiscard]] static SkyboxOperationIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            return SkyboxOperationIds{Ids::fromOps(ops, count)};
        }
    };

    extern LUX_FUNCTION_PUBLIC const FeatureFactory kSkyboxFeatureFactory;

    // =========================================================================
    //  Client-side proxy — SkyboxProxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC SkyboxProxy
    {
    public:
        SkyboxProxy(RenderSession& session, SkyboxOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Set an equirectangular (2D) texture as the skybox.
        void setEquirect(RenderSceneId scene_id, FeatureHandle feature,
                         RTextureHandle texture);

        /// Set a cubemap texture as the skybox.
        void setCubemap(RenderSceneId scene_id, FeatureHandle feature,
                        RTextureHandle cube);

    private:
        RenderSession*     session_;
        SkyboxOperationIds ops_;
    };

} // namespace lux::render
