#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    struct RenderJobTag;
    using RenderJobId = cxx::SlotKey<RenderJobTag>;

    namespace detail
    {
        struct ExecutionBindingId final
        {
            RenderJobId job;
            std::uint64_t generation = 0;
            bool operator==(const ExecutionBindingId&) const noexcept = default;
        };
    }
} // namespace lux::render
