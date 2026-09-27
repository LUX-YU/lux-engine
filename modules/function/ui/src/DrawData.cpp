#include <algorithm>
#include <lux/engine/ui/detail/ImageEncoding.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <imgui.h>
#include <cstring>
#include <vector>

namespace lux::ui
{
    struct DrawData::Impl final
    {
        ImDrawData data;
        std::vector<ImDrawList*> lists;
        std::vector<render::RTextureHandle> textures;

        ~Impl()
        {
            for (auto* list : lists)
                IM_DELETE(list);
        }
    };

    DrawData::DrawData() noexcept = default;
    DrawData::~DrawData() noexcept = default;
    DrawData::DrawData(DrawData&&) noexcept = default;
    DrawData& DrawData::operator=(DrawData&&) noexcept = default;

    bool DrawData::valid() const noexcept
    {
        return impl_ && impl_->data.Valid;
    }

    std::span<const render::RTextureHandle> DrawData::textures() const noexcept
    {
        return impl_ ? std::span<const render::RTextureHandle>{impl_->textures}
                     : std::span<const render::RTextureHandle>{};
    }

    ECaptureError DrawData::captureCurrent() noexcept
    {
        const auto* input = ImGui::GetDrawData();
        if (!input || !input->Valid)
            return ECaptureError::NO_FRAME;
        std::size_t command_count{};
        // Reject borrowed callback state before changing a reusable output.
        for (const auto* list : input->CmdLists)
        {
            command_count += static_cast<std::size_t>(list->CmdBuffer.Size);
            for (const auto& command : list->CmdBuffer)
            {
                if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState)
                    return ECaptureError::UNSUPPORTED_CALLBACK;
                const auto encoded = static_cast<std::uint64_t>(command.GetTexID());
                if (encoded && !detail::decodeImage(encoded).isValid())
                    return ECaptureError::INVALID_INPUT;
            }
        }
        {
            if (!impl_)
                impl_ = std::make_unique<Impl>();
            auto& output = *impl_;
            output.textures.reserve(command_count);
            output.lists.reserve(static_cast<std::size_t>(input->CmdListsCount));
            while (output.lists.size() < static_cast<std::size_t>(input->CmdListsCount))
                output.lists.push_back(IM_NEW(ImDrawList)(nullptr));
            const auto copy = []<class T>(ImVector<T>& target, const ImVector<T>& source) {
                target.resize(source.Size);
                if (source.Size)
                    std::memcpy(target.Data, source.Data, static_cast<std::size_t>(source.Size) * sizeof(T));
            };
            output.textures.clear();
            output.data.CmdLists.resize(input->CmdListsCount);
            for (int index{}; index != input->CmdListsCount; ++index)
            {
                auto& list = *output.lists[index];
                const auto& source = *input->CmdLists[index];
                copy(list.CmdBuffer, source.CmdBuffer);
                copy(list.VtxBuffer, source.VtxBuffer);
                copy(list.IdxBuffer, source.IdxBuffer);
                list.Flags = source.Flags;
                for (auto& command : list.CmdBuffer)
                {
                    command.UserCallbackData = nullptr;
                    command.UserCallbackDataSize = 0;
                    command.UserCallbackDataOffset = 0;
                    const auto texture = detail::decodeImage(static_cast<std::uint64_t>(command.GetTexID()));
                    if (texture.isValid())
                        output.textures.push_back(texture);
                }
                output.data.CmdLists[index] = &list;
            }
            std::sort(output.textures.begin(), output.textures.end(), [](auto a, auto b) noexcept {
                return a.index != b.index ? a.index < b.index : a.gen < b.gen;
            });
            output.textures.erase(std::unique(output.textures.begin(), output.textures.end()), output.textures.end());
            output.data.CmdListsCount = input->CmdListsCount;
            output.data.TotalIdxCount = input->TotalIdxCount;
            output.data.TotalVtxCount = input->TotalVtxCount;
            output.data.DisplayPos = input->DisplayPos;
            output.data.DisplaySize = input->DisplaySize;
            output.data.FramebufferScale = input->FramebufferScale;
            output.data.OwnerViewport = nullptr;
            output.data.Valid = true;
            return ECaptureError::NONE;
        }
    }

    const void* DrawData::nativeDrawData() const noexcept
    {
        return impl_ ? &impl_->data : nullptr;
    }
} // namespace lux::ui
