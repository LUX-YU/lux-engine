#pragma once
#include <atomic>
#include <lux/engine/render/ViewImage.hpp>

namespace lux::render::detail
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
    std::uint64_t texture;
    std::uint64_t generation{};
    std::atomic<std::uint64_t> backing_revision{}, last_submission{}, last_recording{};
};
} // namespace lux::render::detail

namespace lux::render
{
struct ViewImageLease::Record final
{
    std::shared_ptr<detail::ImageVersion> version;
    bool scene_enabled{};
    ImageContentStamp content;
    mutable std::atomic<std::uint64_t> submitted{};
};
} // namespace lux::render

namespace lux::render::detail
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
} // namespace lux::render::detail
