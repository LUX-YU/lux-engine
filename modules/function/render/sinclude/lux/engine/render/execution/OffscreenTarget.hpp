#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/execution/TargetOutput.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;

    // Owns the Target output slices. Contexts only borrow the matching frame slice.
    class LUX_RENDER_PUBLIC OffscreenTarget final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<OffscreenTarget>> create(
            DeviceSession&,
            const TargetDesc&,
            std::uint32_t frame_count
        ) noexcept;
        ~OffscreenTarget() noexcept;
        OffscreenTarget(const OffscreenTarget&) = delete;
        OffscreenTarget& operator=(const OffscreenTarget&) = delete;

        [[nodiscard]] const TargetDesc& description() const noexcept
        {
            return description_;
        }

        [[nodiscard]] DeviceSession& session() const noexcept
        {
            return session_;
        }

        [[nodiscard]] TargetFrame frame(std::uint32_t slot) noexcept
        {
            return frames_[slot];
        }

        [[nodiscard]] TargetOutput output() const noexcept
        {
            return {description_, frames_};
        }

    private:
        OffscreenTarget(DeviceSession&, TargetDesc) noexcept;
        DeviceSession& session_;
        TargetDesc description_;
        std::vector<nri::Texture*> images_;
        std::vector<nri::Descriptor*> views_;
        std::vector<TargetFrame> frames_;
    };
} // namespace lux::render::detail
