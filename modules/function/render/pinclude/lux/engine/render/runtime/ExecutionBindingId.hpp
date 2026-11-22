#pragma once

#include <lux/engine/render/core/Ids.hpp>

namespace lux::render::detail
{
    struct ExecutionBindingId final
    {
        RenderJobId job;
        std::uint64_t generation;

        bool operator==(const ExecutionBindingId&) const noexcept = default;
    };
} // namespace lux::render::detail
