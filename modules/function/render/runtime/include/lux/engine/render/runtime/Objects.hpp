#pragma once

#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/runtime/ResourceDomain.hpp>

namespace lux::render
{
    struct RenderEffectTag;
    struct RenderGraphTag;
    using RenderEffectId = cxx::SlotKey<RenderEffectTag>;
    using RenderGraphId = cxx::SlotKey<RenderGraphTag>;

    struct ViewRef
    {
        RenderSceneId scene;
        RenderViewHandle view;
        bool operator==(const ViewRef&) const noexcept = default;
    };

    struct View
    {
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
    };

    struct RenderEffect
    {
        RenderGraphDefinition definition;
        GraphInvocationData initial_values;
        RenderGraphId graph;
    };

    struct EffectBinding
    {
        ViewRef view;
        RenderTargetId target;
        bool operator==(const EffectBinding&) const noexcept = default;
    };

    // Non-owning schedule value. execution is a Native pool offset, never an object identity.
    struct ExecutionJob
    {
        RenderEffectId effect;
        EffectBinding binding;
        RenderGraphId graph;
        std::uint32_t execution;
    };

} // namespace lux::render
