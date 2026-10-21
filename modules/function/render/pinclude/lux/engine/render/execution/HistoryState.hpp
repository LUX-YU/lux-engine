#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>

namespace lux::render::detail
{
    struct RenderGraphInstance::HistoryState final
    {
        enum class EViewTarget : std::uint8_t
        {
            DESCRIPTOR,
            COLOR,
            DEPTH,
            COLOR_RESOLVE,
            DEPTH_RESOLVE,
            CLEAR
        };

        struct View final
        {
            GraphResourceId resource;
            std::uint32_t pass, index;
            EViewTarget target;
        };

        struct HistorySlot final
        {
            nri::Texture* image = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_size = 0;
            std::vector<nri::Descriptor*> views;
            std::vector<nri::Descriptor*> initial_views;
            SubmissionFrontiers last_use;
            std::uint64_t sample_sequence = 0;
            bool initialized = false;
        };

        void release(const nri::CoreInterface&) noexcept;
        [[nodiscard]] bool select(const DeviceSession&) noexcept;
        void submitted(std::size_t queue, std::uint64_t serial, bool produced) noexcept;

        const HistoryPlan* plan = nullptr;
        std::vector<HistorySlot> slots;
        std::vector<View> views;
        std::uint64_t sequence = 0;
        std::uint32_t latest = kGraphInvalidIndex;
        std::uint32_t previous = 0, current = 1;
        bool invalidate = false;
        bool clear_previous = true, clear_current = true;
    };

} // namespace lux::render::detail
