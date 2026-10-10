#include <lux/engine/render/graph/Definition.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace lux::render
{
    namespace
    {
        constexpr std::array kErrors{
            error::ErrorDescriptor{
                "lux.render.graph.invalid_resource", "Invalid resource declaration (resource)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_use", "Invalid or duplicate resource use (pass, resource)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_dependency", "Invalid dependency (before, after)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.cycle", "Cyclic dependencies (unscheduled pass count)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.missing_producer", "Transient read before write (pass, resource)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_binding", "Invalid frame import (binding position)",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
            }
        };

        bool validUse(const GraphResource& resource, const GraphResourceUse& use) noexcept
        {
            const bool is_valid_access = use.access == EGraphAccess::READ || use.access == EGraphAccess::WRITE ||
                use.access == EGraphAccess::READ_WRITE;
            if (!is_valid_access)
            {
                return false;
            }
            const bool is_image = resource.kind == EGraphResourceKind::IMAGE;
            const bool is_read = use.access == EGraphAccess::READ;
            switch (use.usage)
            {
            case EGraphUsage::TRANSFER:
                return use.access != EGraphAccess::READ_WRITE;
            case EGraphUsage::SHADER:
                return true;
            case EGraphUsage::COLOR_ATTACHMENT:
            case EGraphUsage::DEPTH_ATTACHMENT:
                return is_image;
            case EGraphUsage::VERTEX:
            case EGraphUsage::INDEX:
            case EGraphUsage::UNIFORM:
                return !is_image && is_read;
            case EGraphUsage::PRESENT:
                return is_image && is_read && resource.origin == EGraphResourceOrigin::IMPORTED;
            }
            return false;
        }
    }

    std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept
    {
        return kErrors;
    }

    RenderGraphDefinition::RenderGraphDefinition(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies
    ) noexcept : resources_(std::move(resources)), passes_(std::move(passes)), dependencies_(std::move(dependencies))
    {
    }

    RenderResult<RenderGraphDefinition> RenderGraphDefinition::create(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies
    ) noexcept
    {
        const bool is_too_large = resources.size() > std::numeric_limits<std::uint32_t>::max() ||
            passes.size() > std::numeric_limits<std::uint32_t>::max();
        if (is_too_large)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resources.size()}});
        }
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            const auto& resource = resources[index];
            const bool is_invalid_kind = resource.kind != EGraphResourceKind::BUFFER &&
                resource.kind != EGraphResourceKind::IMAGE;
            const bool is_invalid_origin = resource.origin != EGraphResourceOrigin::TRANSIENT &&
                resource.origin != EGraphResourceOrigin::IMPORTED;
            const bool is_invalid_target = resource.target_semantic.isValid() &&
                resource.kind != EGraphResourceKind::IMAGE;
            const bool is_invalid_resource = is_invalid_kind || is_invalid_origin || is_invalid_target;
            if (is_invalid_resource)
            {
                return cxx::unexpected(RenderError{kGraphInvalidResource, {index + 1}});
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                const bool is_duplicate_target = resource.target_semantic.isValid() &&
                    resource.target_semantic == resources[previous].target_semantic;
                if (is_duplicate_target)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidResource, {index + 1}});
                }
            }
        }
        for (std::size_t index = 0; index < passes.size(); ++index)
        {
            auto& uses = passes[index].uses;
            std::sort(uses.begin(), uses.end(), [](const auto& left, const auto& right) {
                return left.resource.value() < right.resource.value();
            });
            GraphResourceId previous{};
            for (const auto& use : uses)
            {
                const bool is_invalid_id = !use.resource.isValid() || use.resource.value() > resources.size();
                const bool is_duplicate = use.resource == previous;
                const bool is_invalid_use = is_invalid_id || is_duplicate ||
                    !validUse(resources[use.resource.value() - 1], use);
                if (is_invalid_use)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, use.resource.value()}});
                }
                previous = use.resource;
            }
        }
        for (const auto& dependency : dependencies)
        {
            const bool is_invalid_id = !dependency.before.isValid() || !dependency.after.isValid() ||
                dependency.before.value() > passes.size() || dependency.after.value() > passes.size();
            const bool is_self_dependency = dependency.before == dependency.after;
            const bool is_invalid_dependency = is_invalid_id || is_self_dependency;
            if (is_invalid_dependency)
            {
                return cxx::unexpected(RenderError{
                    kGraphInvalidDependency, {dependency.before.value(), dependency.after.value()}
                });
            }
        }
        std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
            if (left.before != right.before)
            {
                return left.before.value() < right.before.value();
            }
            return left.after.value() < right.after.value();
        });
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
        return RenderGraphDefinition{std::move(resources), std::move(passes), std::move(dependencies)};
    }
}
