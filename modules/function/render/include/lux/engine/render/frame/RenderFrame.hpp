#pragma once

#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    class FrameData;

    // A sealed column owner borrowed through terminal completion. No sequence or phase protocol.
    // An empty request runs existing active outputs without applying an input packet.
    struct RenderFrame final
    {
        AttachmentId attachment;
        FrameData* data = nullptr;
        bool resynchronize = false;
    };
} // namespace lux::render
