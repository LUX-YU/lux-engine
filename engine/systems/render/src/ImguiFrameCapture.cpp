#include <lux/engine/systems/render/ImguiFrameCapture.hpp>

namespace lux::engine::render
{
    lux::render::RenderResult<void> captureImguiFrame(
        lux::render::FrameWriter& frame,
        lux::render::FrameDataBinding<lux::render::ImguiFrameData> destination,
        const ImDrawData& data,
        std::span<const lux::render::ImguiImage> images
    ) noexcept
    {
        using namespace lux::render;
        if (!data.Valid || data.CmdListsCount < 0)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        for (const auto* list : data.CmdLists)
        {
            for (const auto& command : list->CmdBuffer)
            {
                const bool unsupported_callback =
                    command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState;
                if (unsupported_callback || command.UserCallbackData)
                {
                    return cxx::unexpected(RenderError{kFrameContract});
                }
            }
        }
        auto lists = frame.allocate<ImguiDrawList>(data.CmdListsCount);
        if (!lists)
        {
            return cxx::unexpected(lists.error());
        }
        for (std::size_t index = 0; index < lists->values.size(); ++index)
        {
            const auto& source = *data.CmdLists[static_cast<int>(index)];
            auto vertices = frame.copy<ImDrawVert>({source.VtxBuffer.Data, std::size_t(source.VtxBuffer.Size)});
            auto indices = frame.copy<ImDrawIdx>({source.IdxBuffer.Data, std::size_t(source.IdxBuffer.Size)});
            auto commands = frame.copy<ImDrawCmd>({source.CmdBuffer.Data, std::size_t(source.CmdBuffer.Size)});
            if (!vertices || !indices || !commands)
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            lists->values[index] = {*vertices, *indices, *commands};
        }
        auto image_range = frame.copy<ImguiImage>(images);
        if (!image_range)
        {
            return cxx::unexpected(image_range.error());
        }
        return frame.write(
            destination,
            {lists->range,
             *image_range,
             {data.DisplayPos.x, data.DisplayPos.y},
             {data.DisplaySize.x, data.DisplaySize.y},
             {data.FramebufferScale.x, data.FramebufferScale.y}}
        );
    }
} // namespace lux::engine::render
