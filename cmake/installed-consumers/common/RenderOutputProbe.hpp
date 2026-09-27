#pragma once
#include <lux/engine/scene/RenderResources.hpp>
#include <cassert>

// Test consumer of the public explicit-reference contract. This is not installed
// and owns no View, scheduling or resource retirement policy.
struct RenderOutputProbe final
{
    lux::scene::RenderResources* resources{};
    lux::scene::RenderResourceId id;
    lux::scene::RenderOutputInfo image;
    RenderOutputProbe() = default;
    RenderOutputProbe(lux::scene::RenderResources& owner, lux::scene::RenderResourceId output)
        : resources(&owner), id(output)
    {
        assert(resources->retain(id));
        auto value = resources->outputInfo(id);
        assert(value);
        image = *value;
    }
    RenderOutputProbe(RenderOutputProbe&& other) noexcept
        : resources(other.resources), id(std::exchange(other.id, {})), image(other.image)
    {}
    RenderOutputProbe& operator=(RenderOutputProbe&& other) noexcept
    {
        if (this != &other)
        {
            if (id.isValid())
                resources->release(id);
            resources = other.resources;
            id = std::exchange(other.id, {});
            image = other.image;
        }
        return *this;
    }
    ~RenderOutputProbe()
    {
        if (id.isValid())
            resources->release(id);
    }
    bool valid() const noexcept
    {
        return id.isValid();
    }
    auto evidence() const noexcept
    {
        return resources->outputInfo(id).transform([](const auto& value) { return value.content; });
    }
    auto capture() const noexcept
    {
        return resources->capture(std::span{&id, 1});
    }
};
inline lux::render::RenderResult<RenderOutputProbe> acquireOutput(lux::scene::RenderResources& resources,
                                                                  lux::scene::RenderResourceId view)
{
    auto output = resources.viewOutput(view);
    if (!output)
        return lux::cxx::unexpected(output.error());
    return RenderOutputProbe(resources, *output);
}
