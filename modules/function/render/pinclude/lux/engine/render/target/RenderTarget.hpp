#pragma once

#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/target/SwapchainTarget.hpp>
#include <variant>

namespace lux::render::runtime_detail
{
    using VRenderTarget = std::variant<detail::OffscreenTarget, detail::SwapchainTarget>;

    [[nodiscard]] inline detail::TargetState& targetState(VRenderTarget& target) noexcept
    {
        return std::visit([](auto& value) noexcept -> detail::TargetState& { return value.state; }, target);
    }

    [[nodiscard]] inline const detail::TargetState& targetState(const VRenderTarget& target) noexcept
    {
        return std::visit([](const auto& value) noexcept -> const detail::TargetState& { return value.state; }, target);
    }

    [[nodiscard]] inline detail::TargetOutput targetOutput(const VRenderTarget& target) noexcept
    {
        return std::visit([](const auto& value) noexcept { return value.output(); }, target);
    }

    [[nodiscard]] inline const TargetDesc& targetDescription(const VRenderTarget& target) noexcept
    {
        return std::visit([](const auto& value) noexcept -> const TargetDesc& { return value.description(); }, target);
    }

    [[nodiscard]] inline bool targetAvailable(const VRenderTarget& target) noexcept
    {
        const auto* swapchain = std::get_if<detail::SwapchainTarget>(&target);
        return !swapchain || swapchain->available();
    }
} // namespace lux::render::runtime_detail
