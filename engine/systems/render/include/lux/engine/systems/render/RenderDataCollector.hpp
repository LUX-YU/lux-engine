#pragma once

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/render/frame/FrameStorage.hpp>

namespace lux::engine::render
{
    // A synchronous batch producer. The frame owns written bytes; it must not retain this object's address.
    class RenderDataCollector : public object::LuxObject
    {
    public:
        ~RenderDataCollector() override = default;

        [[nodiscard]] virtual lux::render::RenderResult<void> collect(lux::render::FrameWriter& frame) noexcept = 0;
        // Dirty collectors restore their authoritative inputs after a rejected/partial frame.
        // Always-collected inputs need no separate recovery state.
        virtual void invalidate() noexcept {}

    };
} // namespace lux::engine::render
