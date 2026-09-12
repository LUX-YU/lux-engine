#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/editor/rendering/detail/RenderFrameQueue.hpp>

namespace lux::editor::rendering
{
    ViewImageLease::ViewImageLease() noexcept = default;
    ViewImageLease::~ViewImageLease() noexcept = default;
    ViewImageLease::ViewImageLease(const ViewImageLease &) noexcept = default;
    ViewImageLease &ViewImageLease::operator=(const ViewImageLease &) noexcept = default;
    ViewImageLease::ViewImageLease(ViewImageLease &&) noexcept = default;
    ViewImageLease &ViewImageLease::operator=(ViewImageLease &&) noexcept = default;
    bool ViewImageLease::valid() const noexcept
    {
        return record_ && record_->version;
    }
    EditorFramePacket::EditorFramePacket() noexcept = default;
    EditorFramePacket::~EditorFramePacket() noexcept = default;
    EditorFramePacket::EditorFramePacket(EditorFramePacket &&) noexcept = default;
    EditorFramePacket &EditorFramePacket::operator=(EditorFramePacket &&) noexcept = default;
    EditorFramePacket::EditorFramePacket(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage))
    {
    }
    bool EditorFramePacket::valid() const noexcept
    {
        return storage_ != nullptr;
    }
    std::uint64_t EditorFramePacket::sequence() const noexcept
    {
        return storage_ ? storage_->sequence : 0;
    }
} // namespace lux::editor::rendering
