#include <lux/engine/ui/DrawData.hpp>

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <vector>

namespace lux::ui
{
    struct DrawData::Impl final
    {
        rdesc::UiDrawData data;
        std::vector<rdesc::UiVertex> vertices;
        std::vector<std::byte> indices;
        std::vector<rdesc::UiDrawCommand> commands;
        std::vector<rdesc::UiImageToken> textures;
        std::pmr::memory_resource* frame_memory = nullptr;
        bool valid = false;

        template <class T> std::span<T> allocate(std::vector<T>& reusable, std::size_t count) noexcept
        {
            if (frame_memory)
            {
                return count
                           ? std::span<T>{static_cast<T*>(frame_memory->allocate(count * sizeof(T), alignof(T))), count}
                           : std::span<T>{};
            }
            reusable.resize(count);
            return reusable;
        }
    };

    DrawData::DrawData() noexcept = default;

    void DrawData::setFrameMemory(std::pmr::memory_resource& memory) noexcept
    {
        if (!impl_)
        {
            impl_ = std::make_unique<Impl>();
        }
        impl_->frame_memory = &memory;
    }

    DrawData::~DrawData() noexcept = default;

    DrawData::DrawData(DrawData&&) noexcept = default;

    DrawData& DrawData::operator=(DrawData&&) noexcept = default;

    bool DrawData::valid() const noexcept
    {
        return impl_ && impl_->valid;
    }

    std::span<const rdesc::UiImageToken> DrawData::textures() const noexcept
    {
        return impl_ ? std::span<const rdesc::UiImageToken>{impl_->textures} : std::span<const rdesc::UiImageToken>{};
    }

    const rdesc::UiDrawData& DrawData::data() const noexcept
    {
        static const rdesc::UiDrawData empty;
        return impl_ ? impl_->data : empty;
    }

    ECaptureError DrawData::captureCurrent() noexcept
    {
        const auto* input = ImGui::GetDrawData();
        if (!input || !input->Valid)
        {
            return ECaptureError::NO_FRAME;
        }
        std::size_t command_count = 0;
        // Validate callback policy before changing a reusable snapshot. Reset is redundant because
        // the renderer executes only these typed draws; arbitrary callbacks never leave Main.
        for (const auto* list : input->CmdLists)
        {
            for (const auto& command : list->CmdBuffer)
            {
                if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState)
                {
                    return ECaptureError::UNSUPPORTED_CALLBACK;
                }
                command_count += !command.UserCallback;
            }
        }
        if (!impl_)
        {
            impl_ = std::make_unique<Impl>();
        }
        auto& output = *impl_;
        auto vertices = output.allocate(output.vertices, static_cast<std::size_t>(input->TotalVtxCount));
        auto indices =
            output.allocate(output.indices, static_cast<std::size_t>(input->TotalIdxCount) * sizeof(ImDrawIdx));
        auto commands = output.allocate(output.commands, command_count);
        output.textures.clear();
        output.textures.reserve(command_count);
        static_assert(sizeof(rdesc::UiVertex) == sizeof(ImDrawVert));
        static_assert(offsetof(rdesc::UiVertex, position) == offsetof(ImDrawVert, pos));
        static_assert(offsetof(rdesc::UiVertex, uv) == offsetof(ImDrawVert, uv));
        static_assert(offsetof(rdesc::UiVertex, color) == offsetof(ImDrawVert, col));
        std::uint32_t first_vertex = 0, first_index = 0;
        std::size_t draw = 0;
        for (const auto* list : input->CmdLists)
        {
            if (list->VtxBuffer.Size)
            {
                std::memcpy(vertices.data() + first_vertex, list->VtxBuffer.Data, list->VtxBuffer.size_in_bytes());
            }
            if (list->IdxBuffer.Size)
            {
                std::memcpy(
                    indices.data() + first_index * sizeof(ImDrawIdx),
                    list->IdxBuffer.Data,
                    list->IdxBuffer.size_in_bytes()
                );
            }
            for (const auto& command : list->CmdBuffer)
            {
                if (command.UserCallback)
                {
                    continue;
                }
                const rdesc::UiImageToken image{static_cast<std::uint64_t>(command.GetTexID())};
                std::construct_at(
                    commands.data() + draw++,
                    rdesc::UiDrawCommand{
                        first_index + command.IdxOffset,
                        command.ElemCount,
                        static_cast<std::int32_t>(first_vertex + command.VtxOffset),
                        {command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w},
                        image
                    }
                );
                if (image.value != 0)
                {
                    output.textures.push_back(image);
                }
            }
            first_vertex += list->VtxBuffer.Size;
            first_index += list->IdxBuffer.Size;
        }
        std::ranges::sort(output.textures);
        output.textures.erase(std::unique(output.textures.begin(), output.textures.end()), output.textures.end());
        output.data = {
            vertices,
            indices,
            commands,
            {input->DisplayPos.x, input->DisplayPos.y},
            {input->DisplaySize.x, input->DisplaySize.y},
            {input->FramebufferScale.x, input->FramebufferScale.y},
            sizeof(ImDrawIdx)
        };
        output.valid = true;
        return ECaptureError::NONE;
    }
} // namespace lux::ui
