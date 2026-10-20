#pragma once

#include <NRI.h>
#include <cstdint>

namespace lux::render::detail
{
    class ExecutionContext;
    void recordGraph(ExecutionContext&, nri::CommandBuffer&, std::uint32_t slot) noexcept;
    void recordGraphPreparation(ExecutionContext&, nri::CommandBuffer&, std::uint32_t slot) noexcept;
} // namespace lux::render::detail
