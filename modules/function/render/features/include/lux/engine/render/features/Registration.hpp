#pragma once

#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/features/Program.hpp>
#include <memory>

namespace lux::render
{
    struct FeatureRegistration final
    {
        const FeatureDefinition* definition;
        RenderResult<std::vector<PassProgram>> (*programs)(const WireView*, std::string_view) noexcept;
        std::span<const CommandHandler> handlers;
        std::shared_ptr<const void> code_pin;
    };

    template <typename T>
    [[nodiscard]] FeatureRegistration nativeFeatureBinding() noexcept;
}
