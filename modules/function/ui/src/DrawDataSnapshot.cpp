#include <algorithm>
#include <cstring>
#include <lux/engine/ui/UiFrameSnapshot.hpp>
#include <lux/engine/ui/detail/ImGuiDrawDataSnapshot.hpp>
#include <memory>
#include <utility>

namespace lux::ui
{
struct UiFrameSnapshot::Impl final
{
    detail::ImGuiDrawDataSnapshot snapshot;
    std::vector<TextureHandle> textures;
};

UiFrameSnapshot::UiFrameSnapshot() noexcept = default;
UiFrameSnapshot::~UiFrameSnapshot() noexcept = default;
UiFrameSnapshot::UiFrameSnapshot(UiFrameSnapshot &&) noexcept = default;
UiFrameSnapshot &UiFrameSnapshot::operator=(UiFrameSnapshot &&) noexcept = default;

bool UiFrameSnapshot::valid() const noexcept
{
    return impl_ != nullptr && impl_->snapshot.drawData().Valid;
}

std::span<const TextureHandle> UiFrameSnapshot::textures() const noexcept
{
    return impl_ ? std::span<const TextureHandle>{impl_->textures} : std::span<const TextureHandle>{};
}

EUiCaptureError UiFrameSnapshot::captureCurrent()
{
    const auto *data = ImGui::GetDrawData();
    if (!data || !data->Valid)
    {
        return EUiCaptureError::NO_FRAME;
    }
    if (!impl_)
    {
        impl_ = std::make_unique<Impl>();
    }
    if (!impl_->snapshot.capture(*data))
    {
        return EUiCaptureError::UNSUPPORTED_CALLBACK;
    }
    impl_->textures.clear();
    for (const auto *list : impl_->snapshot.drawData().CmdLists)
    {
        for (const auto &command : list->CmdBuffer)
        {
            const TextureHandle token{static_cast<std::uint64_t>(command.GetTexID())};
            if (token.valid() && std::ranges::find(impl_->textures, token) == impl_->textures.end())
            {
                impl_->textures.push_back(token);
            }
        }
    }
    return EUiCaptureError::NONE;
}

const void *UiFrameSnapshot::nativeDrawData() const noexcept
{
    return impl_ ? std::addressof(impl_->snapshot.drawData()) : nullptr;
}
} // namespace lux::ui

namespace lux::ui::detail
{
ImGuiDrawDataSnapshot::~ImGuiDrawDataSnapshot()
{
    destroy();
}

ImGuiDrawDataSnapshot::ImGuiDrawDataSnapshot(ImGuiDrawDataSnapshot &&other) noexcept
{
    *this = std::move(other);
}

ImGuiDrawDataSnapshot &ImGuiDrawDataSnapshot::operator=(ImGuiDrawDataSnapshot &&other) noexcept
{
    if (this != std::addressof(other))
    {
        destroy();
        std::swap(draw_data_.Valid, other.draw_data_.Valid);
        std::swap(draw_data_.CmdListsCount, other.draw_data_.CmdListsCount);
        std::swap(draw_data_.TotalIdxCount, other.draw_data_.TotalIdxCount);
        std::swap(draw_data_.TotalVtxCount, other.draw_data_.TotalVtxCount);
        draw_data_.CmdLists.swap(other.draw_data_.CmdLists);
        std::swap(draw_data_.DisplayPos, other.draw_data_.DisplayPos);
        std::swap(draw_data_.DisplaySize, other.draw_data_.DisplaySize);
        std::swap(draw_data_.FramebufferScale, other.draw_data_.FramebufferScale);
        active_lists_ = std::exchange(other.active_lists_, 0);
        owned_lists_ = std::move(other.owned_lists_);
    }
    return *this;
}

bool ImGuiDrawDataSnapshot::capture(const ImDrawData &input)
{
    // Validate before changing a reusable output. Arbitrary callbacks may
    // borrow Context or caller memory; only the built-in reset is portable.
    for (const auto *list : input.CmdLists)
    {
        for (const auto &command : list->CmdBuffer)
        {
            if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState)
            {
                return false;
            }
        }
    }
    while (owned_lists_.size() < static_cast<std::size_t>(input.CmdListsCount))
    {
        owned_lists_.push_back(IM_NEW(ImDrawList)(nullptr));
    }
    const auto copy = []<class T>(ImVector<T> &target, const ImVector<T> &source) {
        target.resize(source.Size);
        if (source.Size != 0)
        {
            std::memcpy(target.Data, source.Data, static_cast<std::size_t>(source.Size) * sizeof(T));
        }
    };
    active_lists_ = input.CmdListsCount;
    for (int index = 0; index < active_lists_; ++index)
    {
        auto &output = *owned_lists_[index];
        const auto &source = *input.CmdLists[index];
        copy(output.CmdBuffer, source.CmdBuffer);
        copy(output.VtxBuffer, source.VtxBuffer);
        copy(output.IdxBuffer, source.IdxBuffer);
        output.Flags = source.Flags;
        // No CPU shared data, viewport, or callback data escapes the frame.
        for (auto &command : output.CmdBuffer)
        {
            command.UserCallbackData = nullptr;
            command.UserCallbackDataSize = 0;
            command.UserCallbackDataOffset = 0;
        }
    }
    draw_data_.Valid = input.Valid;
    draw_data_.TotalIdxCount = input.TotalIdxCount;
    draw_data_.TotalVtxCount = input.TotalVtxCount;
    draw_data_.DisplayPos = input.DisplayPos;
    draw_data_.DisplaySize = input.DisplaySize;
    draw_data_.FramebufferScale = input.FramebufferScale;
    draw_data_.OwnerViewport = nullptr;
    rebuildPointers();
    return true;
}

void ImGuiDrawDataSnapshot::clear() noexcept
{
    active_lists_ = 0;
    draw_data_.Valid = false;
    draw_data_.TotalIdxCount = 0;
    draw_data_.TotalVtxCount = 0;
    draw_data_.CmdListsCount = 0;
    draw_data_.CmdLists.resize(0);
    for (auto *list : owned_lists_)
    {
        list->CmdBuffer.resize(0);
        list->VtxBuffer.resize(0);
        list->IdxBuffer.resize(0);
    }
}

void ImGuiDrawDataSnapshot::destroy() noexcept
{
    for (auto *list : owned_lists_)
    {
        IM_DELETE(list);
    }
    owned_lists_.clear();
    active_lists_ = 0;
    draw_data_.Clear();
}

void ImGuiDrawDataSnapshot::rebuildPointers() noexcept
{
    draw_data_.CmdLists.resize(active_lists_);
    for (int index = 0; index < active_lists_; ++index)
    {
        draw_data_.CmdLists[index] = owned_lists_[index];
    }
    draw_data_.CmdListsCount = active_lists_;
}
} // namespace lux::ui::detail
