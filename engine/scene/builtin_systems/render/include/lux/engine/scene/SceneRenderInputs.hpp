#pragma once

#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneViewRequest.hpp>
#include <lux/engine/scene/render/visibility.h>

namespace lux::scene
{
    struct SceneViewInput final
    {
        SceneViewId id;
        SceneViewRequest request;
        std::uint64_t revision{};
    };

    // Main-thread input values. Borrowing a view never extends its lifetime across a mutation.
    // RenderSystem owns the corresponding Render identities; this collection owns only intent.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC SceneRenderInputs final
    {
    public:
        [[nodiscard]] SceneViewId addView(SceneViewRequest) noexcept;
        [[nodiscard]] bool updateView(SceneViewId, SceneViewRequest) noexcept;
        [[nodiscard]] bool removeView(SceneViewId) noexcept;
        [[nodiscard]] const SceneViewInput* findView(SceneViewId) const noexcept;
        [[nodiscard]] std::span<const SceneViewInput> views() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

        // Asset identity domain supplied by the project, independent of observation changes.
        std::uint64_t asset_revision{};
        std::uint64_t asset_source{
        }; // Fixed before constructing RenderSystem; not changed during that Scene's lifetime.
        asset::AssetVfsView
            asset_snapshot; // Published together with asset_revision; may be empty for a fixed custom port.

    private:
        cxx::SlotMap<SceneViewInput, SceneViewTag> views_;
        std::uint64_t revision_{};
    };
} // namespace lux::scene
