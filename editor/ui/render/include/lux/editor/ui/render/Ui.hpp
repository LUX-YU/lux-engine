#pragma once

#include <array>
#include <cstddef>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/commands/EffectCommands.hpp>
#include <lux/engine/render/inputs/DrawInputs.hpp>
#include <span>
#include <string_view>

namespace lux::render { class RenderServer; }

namespace lux::editor::ui
{
    using namespace lux::render;

    struct UiConfiguration final
    {
        EIndexType index_type = EIndexType::UINT16;
        std::array<float, 4> clear{0.015F, 0.015F, 0.015F, 1}; // Linear BT.709; alpha is coverage.
        [[nodiscard]] bool valid() const noexcept;
        bool operator==(const UiConfiguration&) const noexcept = default;
    };

    inline constexpr std::string_view kUiConfigurationSchema = "lux.render.config.ui";

    [[nodiscard]] RenderResult<SerializedRenderConfiguration> encodeUiConfiguration(const UiConfiguration&) noexcept;
    [[nodiscard]] RenderResult<UiConfiguration> decodeUiConfiguration(std::span<const std::byte>) noexcept;

    [[nodiscard]] RenderResult<CapabilityImplementationId> registerUi(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<CreateEffectRequest>
    makeUiRenderEffect(EffectOutputDesc, const CapabilityImplementationInfo&, const UiConfiguration& = {}) noexcept;
} // namespace lux::editor::ui
