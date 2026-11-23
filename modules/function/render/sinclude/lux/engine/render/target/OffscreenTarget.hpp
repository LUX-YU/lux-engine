#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/runtime/Target.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;

    // Owns the Target output slices. Contexts only borrow the matching frame slice.
    class LUX_RENDER_PUBLIC OffscreenTarget final
    {
    public:
        [[nodiscard]] static RenderResult<OffscreenTarget>
        create(DeviceSession&, const TargetDesc&, std::uint32_t frame_count) noexcept;
        ~OffscreenTarget() noexcept;
        OffscreenTarget(OffscreenTarget&&) noexcept;
        OffscreenTarget(const OffscreenTarget&) = delete;
        OffscreenTarget& operator=(const OffscreenTarget&) = delete;

        TargetState state;

        [[nodiscard]] bool readable(std::uint32_t slot) const noexcept;
        [[nodiscard]] bool writable(std::uint32_t slot) const noexcept;
        void acceptRead(std::uint32_t slot, const SubmissionFrontiers&) noexcept;
        void acceptWrite(std::uint32_t slot, const SubmissionFrontiers&) noexcept;
        void appendUses(SubmissionFrontiers&) const noexcept;
        [[nodiscard]] std::uint64_t publish(std::uint32_t slot) noexcept;

        [[nodiscard]] bool matches(std::uint32_t slot, std::uint64_t revision) const noexcept
        {
            return slot < uses_.size() && uses_[slot].content == revision;
        }

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
        DeviceSession&                  session_;
        TargetDesc                      description_;
        std::vector<nri::Texture*>      images_;
        std::vector<nri::Descriptor*>   views_;
        std::vector<TargetFrame>        frames_;
        std::vector<DescriptorTable::Slot> sampled_slots_;
        struct ImageProgress final
        {
            SubmissionFrontiers all;
            SubmissionFrontiers writes;
            std::uint64_t content = 0;
        };

        std::vector<ImageProgress>           uses_;
        std::uint64_t                   content_revision_ = 0;
    };
} // namespace lux::render::detail
