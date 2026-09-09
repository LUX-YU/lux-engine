#include <lux/engine/ui/detail/ImGuiDrawDataSnapshot.hpp>
#include <lux/engine/ui/UiFrameSnapshot.hpp>
#include <algorithm>
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
    UiFrameSnapshot::UiFrameSnapshot(UiFrameSnapshot&&) noexcept = default;
    UiFrameSnapshot& UiFrameSnapshot::operator=(UiFrameSnapshot&&) noexcept = default;

    bool UiFrameSnapshot::valid() const noexcept
    {
        return impl_ != nullptr && impl_->snapshot.drawData().Valid;
    }

    std::span<const TextureHandle> UiFrameSnapshot::textures() const noexcept
    {
        return impl_ ? std::span<const TextureHandle>{impl_->textures} : std::span<const TextureHandle>{};
    }

    void UiFrameSnapshot::captureCurrent()
    {
        const auto* data = ImGui::GetDrawData();
        if (!data || !data->Valid)
            return;
        auto prepared = std::make_unique<Impl>();
        prepared->snapshot.capture(*data);
        for (const auto* list : prepared->snapshot.drawData().CmdLists)
            for (const auto& command : list->CmdBuffer)
            {
                const TextureHandle token{static_cast<std::uint64_t>(command.GetTexID())};
                if (token.valid() && std::find(prepared->textures.begin(), prepared->textures.end(), token) ==
                    prepared->textures.end())
                    prepared->textures.push_back(token);
            }
        impl_ = std::move(prepared);
    }

    const void* UiFrameSnapshot::nativeDrawData() const noexcept
    {
        return impl_ ? std::addressof(impl_->snapshot.drawData()) : nullptr;
    }
}

namespace lux::ui::detail
{
    ImGuiDrawDataSnapshot::~ImGuiDrawDataSnapshot()
    {
        clear();
    }

    ImGuiDrawDataSnapshot::ImGuiDrawDataSnapshot(ImGuiDrawDataSnapshot&& other) noexcept
        : draw_data_(other.draw_data_), owned_lists_(std::move(other.owned_lists_))
    {
        rebuildPointers();
        other.draw_data_.Clear();
    }

    ImGuiDrawDataSnapshot& ImGuiDrawDataSnapshot::operator=(ImGuiDrawDataSnapshot&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        clear();
        draw_data_ = other.draw_data_;
        owned_lists_ = std::move(other.owned_lists_);
        rebuildPointers();
        other.draw_data_.Clear();
        return *this;
    }

    void ImGuiDrawDataSnapshot::capture(const ImDrawData& draw_data)
    {
        clear();
        draw_data_ = draw_data;
        owned_lists_.reserve(static_cast<std::size_t>(draw_data.CmdListsCount));
        for (int index = 0; index < draw_data.CmdListsCount; ++index)
        {
            auto* copy = draw_data.CmdLists[index]->CloneOutput();
            owned_lists_.push_back(copy);
        }
        rebuildPointers();
    }

    void ImGuiDrawDataSnapshot::clear() noexcept
    {
        for (auto* list : owned_lists_)
            IM_DELETE(list);
        owned_lists_.clear();
        draw_data_.Clear();
    }

    void ImGuiDrawDataSnapshot::rebuildPointers() noexcept
    {
        draw_data_.CmdLists.clear();
        draw_data_.CmdLists.reserve(static_cast<int>(owned_lists_.size()));
        for (auto* list : owned_lists_)
            draw_data_.CmdLists.push_back(list);
        draw_data_.CmdListsCount = draw_data_.CmdLists.Size;
    }
} // namespace lux::ui::detail
