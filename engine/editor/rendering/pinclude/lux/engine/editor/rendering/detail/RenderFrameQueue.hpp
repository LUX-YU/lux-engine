#pragma once
#include <lux/engine/editor/rendering/EditorFramePacket.hpp>
#include <lux/engine/ui/UiFrameSnapshot.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>
#include <vector>

namespace lux::editor::rendering::detail
{
    inline constexpr lux::render::TypeId kUiDrawAttachment = 3;
    struct FrameDrawData final
    {
        lux::ui::UiFrameSnapshot snapshot;
        std::vector<ViewImage> images;
        lux::render::RenderError record_failure;
        std::uint64_t packet_sequence{};
    };
    struct SubmitDrawPayload final
    {
        std::uint32_t attachment{};
    };

    class RenderFrameQueue final
    {
      public:
        explicit RenderFrameQueue(std::size_t capacity) : slots_(capacity)
        {
        }
        bool full() const noexcept
        {
            return size_ == slots_.size();
        }
        bool empty() const noexcept
        {
            return size_ == 0;
        }
        std::size_t size() const noexcept
        {
            return size_;
        }
        void accept(EditorFramePacket &packet) noexcept
        {
            slots_[(head_ + size_++) % slots_.size()] = std::move(packet);
        }
        EditorFramePacket &front() noexcept
        {
            return slots_[head_];
        }
        void pop() noexcept
        {
            slots_[head_] = {};
            head_ = (head_ + 1) % slots_.size();
            --size_;
        }
        void cancel() noexcept
        {
            while (!empty())
                pop();
        }

      private:
        std::vector<EditorFramePacket> slots_;
        std::size_t head_{}, size_{};
    };
} // namespace lux::editor::rendering::detail
namespace lux::editor::rendering
{
    struct EditorFramePacket::Storage final
    {
        std::uint64_t renderer{}, sequence{};
        lux::render::RenderProgram<> program;
    };
} // namespace lux::editor::rendering
