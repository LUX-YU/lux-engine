#include <vulkan/vulkan.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <unordered_map>

namespace
{
    const auto first_device = reinterpret_cast<VkDevice>(1);
    const auto second_device = reinterpret_cast<VkDevice>(2);
    std::unordered_map<VkShaderModule, VkDevice> live;
    std::uintptr_t next_module{100};
    bool reject_create{};
    unsigned attempts{}, destroyed{};

    VkResult createModule(
        VkDevice device,
        const VkShaderModuleCreateInfo* info,
        const VkAllocationCallbacks*,
        VkShaderModule* output
    )
    {
        ++attempts;
        assert(info->codeSize != 0 && info->pCode != nullptr);
        *output = VK_NULL_HANDLE;
        if (reject_create)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        *output = reinterpret_cast<VkShaderModule>(++next_module);
        assert(live.emplace(*output, device).second);
        return VK_SUCCESS;
    }

    void destroyModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks*)
    {
        const auto it = live.find(module);
        assert(it != live.end() && it->second == device);
        live.erase(it);
        ++destroyed;
    }
} // namespace

// Compile the real cache, identity and ownership paths; inject only the native boundary.
// clang-format off
#define vkCreateShaderModule createModule
#define vkDestroyShaderModule destroyModule
#include "../src/resources/ShaderResources.cpp"
#undef vkDestroyShaderModule
#undef vkCreateShaderModule
// clang-format on

int main()
{
    using namespace lux::render;
    const std::array<uint32_t, 5> words{0x07230203, 0x10000, 0, 1, 0};
    const auto bytes = std::as_bytes(std::span(words));
    const lux::rdesc::ShaderInfo reflection{};
    {
        ShaderResources source;
        ShaderResources destination;
        source.init({first_device});
        destination.init({second_device});
        reject_create = true;
        assert(source.add(bytes, reflection).isNull() && live.empty());
        reject_create = false;
        const auto handle = source.add(bytes, reflection);
        const auto count = attempts;
        const auto same = source.add(bytes, reflection);
        assert(handle == same && attempts == count && live.size() == 1);
        source.remove(handle);
        assert(source.get(same) != nullptr && live.size() == 1);
        const auto other = destination.add(bytes, reflection);
        assert(!other.isNull() && live.size() == 2);
        destination = std::move(source);
        if (live.size() != 1)
        {
            std::fprintf(stderr, "FAIL: move assignment left %zu shader modules alive; expected 1\n", live.size());
            return 1;
        }
        assert(destination.get(same) != nullptr);
        assert(destination.spirvBytes(same).size() == bytes.size());
        ShaderResources moved(std::move(destination));
        moved.remove(same);
        assert(moved.get(same) == nullptr && live.empty());
        moved.remove(same);
        const auto replacement = moved.add(bytes, reflection);
        assert(!replacement.isNull() && replacement != same);
        assert(moved.get(same) == nullptr && live.size() == 1);
    }
    assert(live.empty() && destroyed == 3);
}
