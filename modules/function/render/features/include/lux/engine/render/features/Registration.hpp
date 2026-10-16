#pragma once

#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <memory>
#include <lux/engine/render/resource/Registration.hpp>

namespace lux::render
{
    struct ProgramContribution final
    {
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
    };


    struct FeatureRegistration final
    {
        const FeatureDescriptor* descriptor;
        const FeatureGraphOps* graph;
        RenderResult<ProgramContribution> (*programs)(const WireView*, std::string_view) noexcept;
        std::span<const CommandRegistration> handlers;
        std::shared_ptr<const void> code_pin;
    };

    template <typename T>
    [[nodiscard]] FeatureRegistration nativeFeatureBinding() noexcept;
}
