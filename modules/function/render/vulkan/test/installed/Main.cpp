#include <array>
#include <lux/engine/render/vulkan/shader/Layout.hpp>

int main()
{
    using namespace lux::render::vulkan;
    OwnerShape first{
        {},
        "light.main",
        1,
        lux::rdesc::EFieldOwner::SCENE,
        {{"records", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 4, 0, 32, 16, {}, {}, "abi"}}
    };
    first.identity = shaderOwnerId(first);
    auto second = first;
    second.canonical_name = "light.preview";
    second.identity = shaderOwnerId(second);
    if (!first.identity.isValid() || first.identity == second.identity)
    {
        return 1;
    }
    std::array<OwnerAssignment, 2> slots{{{"main", first.identity}, {"preview", second.identity}}};
    return slots[0].owner != slots[1].owner ? 0 : 2;
}
