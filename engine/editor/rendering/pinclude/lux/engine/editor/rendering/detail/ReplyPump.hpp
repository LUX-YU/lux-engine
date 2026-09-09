#pragma once
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>

namespace lux::editor::rendering::detail
{
    inline std::size_t pumpRendererReplies(lux::render::RenderControlSession &control,
                                           lux::render::RenderProgramSession &program,
                                           lux::render::RenderUploadSession &upload, std::size_t &next_lane,
                                           std::size_t budget)
    {
        std::size_t count{}, empty_lanes{};
        while (count < budget && empty_lanes < 3)
        {
            const auto lane = next_lane;
            next_lane = (lane + 1) % 3;
            const auto consumed = lane == 0   ? control.pumpReplies(1)
                                  : lane == 1 ? program.pumpReplies(1)
                                              : upload.pumpReplies(1);
            count += consumed;
            empty_lanes = consumed ? 0 : empty_lanes + 1;
        }
        return count;
    }
} // namespace lux::editor::rendering::detail
