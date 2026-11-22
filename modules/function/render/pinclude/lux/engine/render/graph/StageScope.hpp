#pragma once

#include <NRI.h>
#include <cstdint>

namespace lux::render::detail
{
    // NRI encodes ALL as zero. Once a selected scope is ALL, union must preserve it.
    [[nodiscard]] inline nri::StageBits combineStages(nri::StageBits a, nri::StageBits b) noexcept
    {
        if (a == nri::StageBits::NONE) return b;
        if (b == nri::StageBits::NONE) return a;
        return a == nri::StageBits::ALL || b == nri::StageBits::ALL ? nri::StageBits::ALL : a | b;
    }

    [[nodiscard]] inline bool coversStages(nri::StageBits offered, nri::StageBits required) noexcept
    {
        if (required == nri::StageBits::NONE || offered == nri::StageBits::ALL) return true;
        if (offered == nri::StageBits::NONE || required == nri::StageBits::ALL) return false;
        return (offered & required) == static_cast<std::uint32_t>(required);
    }

    [[nodiscard]] inline bool coversScope(nri::AccessStage offered, nri::AccessStage required) noexcept
    {
        const bool covers_access = (offered.access & required.access) == static_cast<std::uint32_t>(required.access);
        return covers_access && coversStages(offered.stages, required.stages);
    }
} // namespace lux::render::detail
