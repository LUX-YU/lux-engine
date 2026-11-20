#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render::detail
{
    [[nodiscard]] constexpr RenderError
    descriptorLimit(EDescriptorLimit kind, std::uint64_t requested, std::uint64_t available) noexcept
    {
        return {kDescriptorLimitExceeded, {requested, available, static_cast<std::uint64_t>(kind)}};
    }

    [[nodiscard]] constexpr RenderError
    deviceBudget(EDeviceBudget kind, std::uint64_t used, std::uint64_t budget) noexcept
    {
        return {kDeviceBudgetExceeded, {used, budget, static_cast<std::uint64_t>(kind)}};
    }

    // Preserve the exact NRI result at the backend boundary; messages never determine control flow.
    [[nodiscard]] constexpr RenderError nativeError(nri::Result result, std::uint64_t operation = 0) noexcept
    {
        return {kDeviceFailure, {static_cast<std::uint32_t>(result), operation}};
    }

    [[nodiscard]] constexpr bool isNativeFailure(const RenderError& error, nri::Result result) noexcept
    {
        return error.type == kDeviceFailure && error.args[0] == nativeError(result).args[0];
    }

} // namespace lux::render::detail
