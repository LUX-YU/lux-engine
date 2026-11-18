#pragma once

#include <lux/engine/render/core/EffectParameters.hpp>
#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/resource/Binding.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderEffect final
    {
        explicit RenderEffect(EffectParameters parameters) noexcept : parameters_(parameters) {}

        RenderGraph graph;
        RenderGraphAnalysis analysis;
        EffectOutputDesc output;
        std::vector<std::uint32_t> parameter_groups;

        struct ResourceDependency final
        {
            EResourceDomain scope;
            ResourceTypeId declaration;
            std::vector<StorageArgument> bindings;
            std::vector<std::uint32_t> columns;
        };

        std::vector<ResourceDependency> resource_dependencies;
        FrameLayout input_schema;
        std::vector<CommandBindingLocation> commands;

        struct ExternalImage final
        {
            FeaturePortRef destination;
            GraphResourceId resource;
        };
        std::vector<ExternalImage> external_images;

        [[nodiscard]] std::uint64_t parameterRevision() const noexcept { return parameter_revision_; }
        [[nodiscard]] std::span<const std::byte> parameterBytes() const noexcept
        {
            return std::as_bytes(std::span{&parameters_, 1});
        }
        [[nodiscard]] RenderResult<void> setParameters(EffectParameters) noexcept;

        // The same lifetime borrow covers asynchronous compilation and the published execution.
        [[nodiscard]] bool tryBorrowExecution() noexcept;
        void releaseExecution() noexcept;
        [[nodiscard]] bool hasExecutions() const noexcept { return execution_borrows_ != 0; }

    private:
        EffectParameters parameters_;
        std::uint64_t parameter_revision_ = 1;
        std::uint32_t execution_borrows_ = 0;
    };
} // namespace lux::render::runtime_detail
