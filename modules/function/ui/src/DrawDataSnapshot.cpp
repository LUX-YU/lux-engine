#include <lux/engine/ui/detail/ImGuiDrawDataSnapshot.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>

#include <memory>
#include <utility>

namespace lux::ui::detail
{
    struct UiDrawDataSnapshot::Impl final
    {
        ImGuiDrawDataSnapshot snapshot;
    };

    UiDrawDataSnapshot::UiDrawDataSnapshot() : impl_(std::make_unique<Impl>()) {}
    UiDrawDataSnapshot::~UiDrawDataSnapshot() = default;
    UiDrawDataSnapshot::UiDrawDataSnapshot(UiDrawDataSnapshot&&) noexcept = default;
    UiDrawDataSnapshot& UiDrawDataSnapshot::operator=(UiDrawDataSnapshot&&) noexcept = default;

    bool UiDrawDataSnapshot::valid() const noexcept
    {
        return impl_ != nullptr && impl_->snapshot.drawData().Valid;
    }

    void UiDrawDataSnapshot::captureCurrent()
    {
        const auto* draw_data = ImGui::GetDrawData();
        if (draw_data != nullptr && draw_data->Valid)
            impl_->snapshot.capture(*draw_data);
    }

    const void* UiDrawDataSnapshot::nativeDrawData() const noexcept
    {
        return impl_ == nullptr ? nullptr : std::addressof(impl_->snapshot.drawData());
    }

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
