#pragma once
#include <lux/engine/editor/rendering/ViewImage.hpp>
#include <lux/engine/function/render/client/features/view_camera/ViewCameraOperation.hpp>
#include <atomic>

namespace lux::editor::rendering::detail
{
    // A real target storage version. RenderView retains the control leases until all CPU users
    // release this version AND the backend's fence-proven watermark covers last_submission.
    struct ImageVersion final
    {
        RenderViewId id;
        lux::render::RenderSceneId scene;
        lux::render::ViewHandle view;
        lux::render::RenderTargetId target;
        PixelExtent extent;
        lux::ui::TextureHandle texture;
        std::uint64_t generation{};
        std::atomic<std::uint64_t> backing_revision{}, last_submission{}, last_recording{};
    };
} // namespace lux::editor::rendering::detail

namespace lux::editor::rendering
{
    struct ViewImageLease::Record final
    {
        std::shared_ptr<detail::ImageVersion> version;
        CameraFrame camera;
        lux::render::ViewCameraUpdatePayload wire_camera;
        ImageContentStamp content;
        mutable std::atomic<std::uint64_t> submitted{};
    };
} // namespace lux::editor::rendering

namespace lux::editor::rendering::detail
{
    struct ViewImageAccess final
    {
        static long references(const ViewImage &image) noexcept
        {
            return image.lease.record_.use_count();
        }
        static const auto *record(const ViewImage &image) noexcept
        {
            return image.lease.record_.get();
        }
        static bool sameRecord(const ViewImage &a, const ViewImage &b) noexcept
        {
            return a.lease.record_ == b.lease.record_;
        }
    };
} // namespace lux::editor::rendering::detail
