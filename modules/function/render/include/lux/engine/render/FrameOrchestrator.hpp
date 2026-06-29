#pragma once

#include <lux/engine/render/FrameStamp.hpp>

namespace lux::render
{

class Renderer;

class FrameOrchestrator
{
public:
    explicit FrameOrchestrator(uint32_t frames_in_flight)
        : frame_clock_(frames_in_flight)
    {
    }

    [[nodiscard]] FrameStamp beginTick(uint32_t image_index_hint = 0)
    {
        current_stamp_ = frame_clock_.beginTick(image_index_hint);
        return current_stamp_;
    }

    void patchImageIndex(uint32_t image_index) noexcept
    {
        current_stamp_.image_index = image_index;
    }

    [[nodiscard]] const FrameStamp& stamp() const noexcept { return current_stamp_; }
    [[nodiscard]] uint32_t framesInFlight() const noexcept { return frame_clock_.framesInFlight(); }

    void beginFrame(Renderer& renderer) const;
    void endFrame(Renderer& renderer) const;

private:
    FrameClock frame_clock_;
    FrameStamp current_stamp_{};
};

} // namespace lux::render
