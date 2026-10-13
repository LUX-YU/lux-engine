#pragma once

#include <lux/engine/render/core/Handles.hpp>
#include <lux/engine/render/graph/Bindings.hpp>

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
        bool active{true};
    };

    struct Scene
    {
        cxx::SlotMap<View, RenderViewTag> views;
    };

    struct Target
    {
        std::uint32_t width{}, height{};
    };

    enum class EResourceScope
    {
        GLOBAL,
        SCENE,
        VIEW,
        ATTACHMENT
    };

    // Explicit cold declarations for R3-A's small Buffer slice. ResourcePlan compilation is R3-B.
    struct ScopedBufferRequest
    {
        GraphResourceId resource;
        EResourceScope scope;
        std::string key;
    };

    struct RenderEffect
    {
        RenderGraphDefinition definition;
        GraphInvocationData initial_values;
        RenderGraphId graph;
        std::vector<ScopedBufferRequest> buffers;
    };

    struct EffectBinding
    {
        ViewRef view;
        RenderTargetId target;
        bool operator==(const EffectBinding&) const noexcept = default;
    };

} // namespace lux::render
