#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>

#include <cassert>
#include <string_view>
#include <vector>

int main()
{
    const auto registrations = lux::render::builtinRenderFeatureRegistrations();
    assert(registrations.size() == 34U);
    std::size_t non_scene_configurable{};
    for (std::size_t index{}; index < registrations.size(); ++index)
    {
        const auto& registration = registrations[index];
        assert(!registration.stable_name.empty());
        assert(registration.descriptor != nullptr && registration.descriptor->valid());
        assert(lux::render::featureId(registration.stable_name) == registration.descriptor->type);
        assert(registration.configuration.valid());
        if (!registration.scene_configurable) ++non_scene_configurable;
        std::vector<std::byte> portable;
        assert(registration.configuration.portable.encode_default(portable));
        std::vector<std::byte> attach;
        assert(registration.configuration.materialize_attach(portable, attach));
        assert(attach.size() == registration.configuration.attach_wire_size);
        for (std::size_t previous{}; previous < index; ++previous)
        {
            assert(registrations[previous].descriptor->type != registration.descriptor->type);
            assert(registrations[previous].stable_name != registration.stable_name);
            assert(registrations[previous].descriptor->name != registration.descriptor->name);
        }
    }
    assert(non_scene_configurable == 5U);
    return 0;
}
