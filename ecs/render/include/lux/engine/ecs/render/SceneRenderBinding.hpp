#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <lux/engine/function/visibility.h>

#include <span>

#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Grid.hpp>

namespace lux::render {
    class RenderFrameSession;
    class RenderControlSession;
    struct FeatureAttach;
}

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC SceneRenderBinding
    {
    public:
        SceneRenderBinding(
            lux::render::RenderFrameSession&        session,
            lux::render::RenderControlSession& control,
            lux::render::RenderUploadClient    upload
        ) noexcept
            : session_(session), control_(control), upload_(std::move(upload)),
              scene_{} {}

        SceneRenderBinding(
            lux::render::RenderFrameSession& session,
            lux::render::RenderControlSession& control,
            lux::render::RenderUploadClient upload,
            lux::render::RenderSceneId scene_id
        ) noexcept
            : SceneRenderBinding(session, control, std::move(upload))
        {
            (void)bindScene(scene_id);
        }

        /// Complete the unpublished binding after local topology preflight
        /// and remote scene creation. The scene identity is cold one-shot
        /// state; published systems never replace it.
        [[nodiscard]] bool bindScene(
            lux::render::RenderSceneId scene_id) noexcept
        {
            if (!scene_.isNull() || scene_id.isNull())
                return false;
            scene_ = scene_id;
            return true;
        }

        [[nodiscard]] lux::render::RenderFrameSession& session() noexcept
        {
            return session_.get();
        }
        [[nodiscard]] lux::render::RenderControlSession& control() noexcept
        {
            return control_.get();
        }
        [[nodiscard]] const lux::render::RenderUploadClient& upload() const noexcept
        {
            return upload_;
        }
        [[nodiscard]] lux::render::RenderSceneId   scene()   const noexcept { return scene_; }

        [[nodiscard]] const lux::math::GridCoord3i64&
        sceneOriginTile3D() const noexcept
        {
            return scene_origin_tile_3d_;
        }

        void requestSceneOriginRebase(
            const lux::math::Position3d& position) noexcept;

        void requestSceneOriginRebase(
            const lux::math::Position2d& position) noexcept;

        [[nodiscard]] bool applyPendingSceneOriginRebase() noexcept;

        [[nodiscard]] lux::render::RenderCapabilities features() const noexcept
        {
            return {
                catalog_ ? &catalog_->get() : nullptr,
                &bindings_
            };
        }

        [[nodiscard]] const lux::render::FeatureCatalog* catalog() const noexcept
        {
            return catalog_ ? &catalog_->get() : nullptr;
        }

        /// Publish the complete renderer capability view exactly once. The
        /// binding remains mutable only for non-topology scene state such as
        /// origin rebasing; FeatureCatalog and handles are immutable after
        /// this cold assembly seal.
        [[nodiscard]] bool seal(
            const lux::render::FeatureCatalog& catalog,
            lux::render::FeatureBindings bindings = {}) noexcept
        {
            if (catalog_)
                return false;
            catalog_ = std::cref(catalog);
            bindings_ = std::move(bindings);
            return true;
        }

        [[nodiscard]] lux::render::MeshStackProxy meshStack() noexcept
        {
            return { session_.get(),
                     features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack") };
        }
        [[nodiscard]] lux::render::SkinningOperationIds skinningOps() const noexcept
        {
            return features().ops<lux::render::SkinningOperationIds>("Skinning");
        }
        [[nodiscard]] lux::render::Canvas2DProxy canvas2d() noexcept
        {
            return { session_.get(), features().ops<lux::render::Canvas2DOperationIds>("Canvas2D") };
        }


    private:
        std::reference_wrapper<lux::render::RenderFrameSession> session_;
        std::reference_wrapper<lux::render::RenderControlSession> control_;
        lux::render::RenderUploadClient upload_;
        lux::render::RenderSceneId scene_{};
        lux::math::GridCoord3i64 scene_origin_tile_3d_{};
        std::optional<lux::math::GridCoord3i64>
            pending_scene_origin_tile_;
        std::optional<std::reference_wrapper<
            const lux::render::FeatureCatalog>> catalog_;
        lux::render::FeatureBindings      bindings_{};
    };

} // namespace lux::ecs

namespace lux::ecs
{
    struct FeatureSettleReport
    {
        enum class Status : std::uint8_t
        {
            OK,
            CHANNEL_STOPPED,
            ATTACH_REJECTED,
            DISPATCH_FAILED,
            BINDING_ALREADY_SEALED
        };

        Status status{Status::OK};
        std::string_view rejected{};
        lux::render::RenderError error{};
        lux::render::FeatureCatalog::ResolveOutcome resolve;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC FeatureSettleReport settleRenderCapabilities(
        SceneRenderBinding&                     ctx,
        const lux::render::FeatureCatalog&       catalog,
        std::span<const lux::render::FeatureAttach> plan,
        std::span<const std::string_view>           roots,
        std::string_view                            profile);

} // namespace lux::ecs
