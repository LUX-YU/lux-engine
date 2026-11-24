#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/runtime/Target.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/resource/ResourceBinding.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <memory>
#include <optional>
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

        // The round retains this concrete image through settlement. Only an earlier publication
        // needs a producer fence here; current-run ordering comes from the product dependency.
        struct SelectedImage final
        {
            ResourceInput input{};
            DescriptorTable::Slot view{};
        };
        [[nodiscard]] SelectedImage selectRead(std::optional<std::uint32_t> writer_image) noexcept;
        [[nodiscard]] std::uint32_t writableImage(std::uint32_t preferred) const noexcept;
        [[nodiscard]] std::uint32_t publishedImage() const noexcept { return published_image_; }
        void bindOutput(std::uint32_t frame_slot, std::uint32_t image_index) noexcept;

        void publish(std::uint32_t image) noexcept;
        void appendUses(SubmissionFrontiers&) const noexcept;

        [[nodiscard]] const TargetDesc& description() const noexcept
        {
            return description_;
        }

        [[nodiscard]] DeviceSession& session() const noexcept
        {
            return session_;
        }

        [[nodiscard]] TargetFrame frame(std::uint32_t slot) const noexcept
        {
            return frames_[slot];
        }

        [[nodiscard]] TargetOutput output() const noexcept
        {
            return {description_, frame_outputs_};
        }

    private:
        OffscreenTarget(DeviceSession&, TargetDesc) noexcept;
        [[nodiscard]] bool writable(std::uint32_t slot) const noexcept;
        DeviceSession&                  session_;
        TargetDesc                      description_;
        std::vector<nri::Texture*>      images_;
        std::vector<nri::Descriptor*>   views_;
        std::vector<TargetFrame>        frames_;
        std::vector<TargetFrame>        frame_outputs_;
        std::uint32_t                   published_image_ = UINT32_MAX;
        std::vector<DescriptorTable::Slot> sampled_slots_;
        struct ImageProgress final
        {
            SubmissionFrontiers all;
            SubmissionFrontiers writes;
        };

        std::vector<ImageProgress>           uses_;
    };
} // namespace lux::render::detail
