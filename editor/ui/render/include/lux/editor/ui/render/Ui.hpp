#pragma once

#include <array>
#include <cstddef>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/runtime/commands/EffectCommands.hpp>
#include <lux/engine/render/graph/Draw.hpp>
#include <span>
#include <string_view>

namespace lux::render { class RenderServer; }

namespace lux::editor::ui
{
    struct UiConfiguration final
    {
        render::EIndexType index_type = render::EIndexType::UINT16;
        std::array<float, 4> clear{0.015F, 0.015F, 0.015F, 1}; // Linear BT.709; alpha is coverage.
        [[nodiscard]] bool valid() const noexcept;
        bool operator==(const UiConfiguration&) const noexcept = default;
    };

    inline constexpr std::string_view kUiConfigurationSchema = "lux.render.config.ui";

    [[nodiscard]] render::RenderResult<render::SerializedRenderConfiguration>
    encodeUiConfiguration(const UiConfiguration&) noexcept;
    [[nodiscard]] render::RenderResult<UiConfiguration> decodeUiConfiguration(std::span<const std::byte>) noexcept;

    [[nodiscard]] render::RenderResult<render::CapabilityImplementationId> registerUi(render::RenderServer&) noexcept;
    [[nodiscard]] render::RenderResult<render::CreateEffectRequest>
    makeUiRenderEffect(
        render::EffectOutputDesc, const render::CapabilityImplementationInfo&, const UiConfiguration& = {}
    ) noexcept;
} // namespace lux::editor::ui
