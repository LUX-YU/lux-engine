#pragma once

#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>
#include <lux/engine/render/graph/StorageBindingSlot.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::compiled
{
    struct BindingOperand final
    {
        enum class EDomain : std::uint8_t { TARGET, STORAGE };
        EDomain domain;
        std::uint32_t slot;
    };

    struct BindingComparison final
    {
        BindingOperand left, right;
    };

    // Published together after all device programs, preparation operands and imports have linked.
    struct RenderGraph final
    {
        detail::RenderGraphExecutable executable;
        detail::NativeProgramBindings programs;
        std::vector<detail::TargetImageSlot> target_imports;
        std::vector<detail::StorageBindingSlot> storage_imports;
        std::vector<detail::FinalBufferUse> output_uses;
        std::vector<detail::FinalBufferExport> buffer_exports;
        std::vector<BindingComparison> binding_comparisons;
        detail::PreparationPlan preparation;
        std::vector<detail::LinkedUniformBlock> uniforms;
    };
} // namespace lux::render::compiled
