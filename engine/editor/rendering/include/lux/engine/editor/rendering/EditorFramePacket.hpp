#pragma once
#include <lux/engine/editor/rendering/ViewImage.hpp>
namespace lux::editor::rendering
{
    namespace detail
    {
    }
    class LUX_EDITOR_RENDERING_PUBLIC EditorFramePacket final
    {
      public:
        EditorFramePacket() noexcept;
        ~EditorFramePacket() noexcept; // Releases unsubmitted ownership; never submits work.
        EditorFramePacket(EditorFramePacket &&) noexcept;
        EditorFramePacket &operator=(EditorFramePacket &&) noexcept;
        EditorFramePacket(const EditorFramePacket &) = delete;
        EditorFramePacket &operator=(const EditorFramePacket &) = delete;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::uint64_t sequence() const noexcept;

      private:
        friend class EditorRenderer;
        struct Storage;
        explicit EditorFramePacket(std::unique_ptr<Storage>) noexcept;
        std::unique_ptr<Storage> storage_;
    };
} // namespace lux::editor::rendering
