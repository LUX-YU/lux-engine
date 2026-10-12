#pragma once

#include <lux/engine/render/core/Handles.hpp>
#include <lux/engine/render/graph/Bindings.hpp>

namespace lux::render
{
    struct RenderEffectTag;
    struct RenderGraphTag;
    struct ResourceDomainTag;
    using RenderEffectId = cxx::SlotKey<RenderEffectTag>;
    using RenderGraphId = cxx::SlotKey<RenderGraphTag>;
    using ResourceDomainId = cxx::SlotKey<ResourceDomainTag>;

    struct ViewRef
    {
        RenderSceneId scene;
        RenderViewHandle view;
        bool operator==(const ViewRef&) const noexcept = default;
    };

    struct View
    {
        RenderEffectId effect;
        RenderGraphId graph;
        RenderTargetId target;
        ResourceDomainId domain;
        bool active{true};
    };

    struct Scene
    {
        cxx::SlotMap<View, RenderViewTag> views;
        ResourceDomainId domain;
    };

    struct Target
    {
        std::uint32_t width{}, height{};
        std::optional<ViewRef> view;
    };

    struct ResourceDomain
    {
        ResourceDomainId parent;
    };

    struct RenderEffect
    {
        RenderGraphDefinition definition;
        GraphInvocationData initial_values;
        RenderGraphId graph;
    };

    // Non-owning schedule value. execution is a Native pool offset, never an object identity.
    struct ExecutionJob
    {
        ViewRef view;
        RenderGraphId graph;
        std::uint32_t execution;
    };

    struct GraphViewGroup
    {
        std::vector<ExecutionJob> jobs;
    };
} // namespace lux::render
