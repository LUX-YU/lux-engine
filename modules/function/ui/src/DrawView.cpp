#include <algorithm>
#include <imgui.h>
#include <lux/engine/ui/DrawView.hpp>

namespace lux::ui
{
    bool DrawView::valid() const noexcept
    {
        return data_ != nullptr;
    }

    const ImDrawData& DrawView::data() const noexcept
    {
        return *data_;
    }

    ECaptureError DrawView::borrowCurrent() noexcept
    {
        data_ = ImGui::GetDrawData();
        textures_ready_ = false;
        if (!data_ || !data_->Valid)
        {
            data_ = nullptr;
            return ECaptureError::NO_FRAME;
        }
        return ECaptureError::NONE;
    }

    std::span<const rdesc::UiImageToken> DrawView::textures() const noexcept
    {
        if (!textures_ready_)
        {
            textures_.clear();
            if (data_)
            {
                for (const auto* list : data_->CmdLists)
                {
                    for (const auto& command : list->CmdBuffer)
                    {
                        if (!command.UserCallback && command.GetTexID() != 0)
                        {
                            textures_.push_back({static_cast<std::uint64_t>(command.GetTexID())});
                        }
                    }
                }
                std::ranges::sort(textures_);
                textures_.erase(std::unique(textures_.begin(), textures_.end()), textures_.end());
            }
            textures_ready_ = true;
        }
        return textures_;
    }
} // namespace lux::ui
