#pragma once

#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/render/commands/Error.hpp>

namespace lux::render::detail
{
    class CameraInputHandler final
    {
    public:
        static RenderResult<void> apply(CameraInput&, std::uint64_t&, const CameraInput&) noexcept;
    };
} // namespace lux::render::detail
