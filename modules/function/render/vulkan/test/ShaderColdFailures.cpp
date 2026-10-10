#include "F3SharedA.pass.hpp"
#include "F3Storage.pass.hpp"
#include "ShaderTestSupport.hpp"
#include <lux/engine/render/vulkan/shader/Format.hpp>
#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>

using namespace foundation_test;
using namespace shader_test;
using namespace lux::toolchain;

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::filesystem::path binaries(argv[1]);
    Validation validation;
    {
        auto host = instance(validation);
        auto device = take(VulkanDevice::create(host));
        const auto caps = queryLayoutCaps(device);
        const auto schema = PassSchema<F3Storage>::contract();
        const std::array schemas{schema};
        const std::array owners{
            take(declareOwnerShape("scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas)),
            take(declareOwnerShape("feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas)),
            take(declareOwnerShape("pass", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas))
        };
        const OwnerAssignment assignment{owners[0].identity, owners[1].identity, owners[2].identity};
        const auto plan = take(compileLayout(schema, assignment, owners, caps));
        std::vector<PassDescriptorLocation> locations;
        for (const auto& field : plan.identity().fields)
        {
            locations.push_back({field.field_index, field.set, field.binding});
        }
        unsigned rejected = 0;
        for (const auto member :
             {&VkPhysicalDeviceLimits::maxBoundDescriptorSets,
              &VkPhysicalDeviceLimits::maxPushConstantsSize,
              &VkPhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers,
              &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers,
              &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageImages,
              &VkPhysicalDeviceLimits::maxPerStageResources,
              &VkPhysicalDeviceLimits::maxDescriptorSetUniformBuffers,
              &VkPhysicalDeviceLimits::maxDescriptorSetStorageBuffers,
              &VkPhysicalDeviceLimits::maxDescriptorSetStorageImages,
              &VkPhysicalDeviceLimits::maxUniformBufferRange,
              &VkPhysicalDeviceLimits::maxStorageBufferRange})
        {
            auto reduced = caps;
            reduced.limits.*member = 0;
            CHECK(!compileLayout(schema, assignment, owners, reduced));
            ++rejected;
        }
        for (const auto flag :
             {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
              VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT})
        {
            auto modified = owners;
            modified[0].fields[0].flags = flag;
            auto result = compileLayout(schema, assignment, modified, caps);
            CHECK(!result && result.error().type == kUnsupported);
            ++rejected;
        }
        auto modified = owners;
        modified[0].fields[0].element_alignment = 3;
        CHECK(!compileLayout(schema, assignment, modified, caps));
        ++rejected;
        std::printf(
            "G08 cold rejections=%u; lowered snapshots are synthetic boundary inputs, actual queried device=%s\n",
            rejected,
            device.properties().deviceName
        );

        const auto original = words(binaries / "F3Storage.comp.spv");
        CHECK(relocatePassSpirv(original, schema, locations, 4));
        const auto reject = [&](std::vector<std::uint32_t> value, const char* label)
        {
            auto result = relocatePassSpirv(value, schema, locations, 4);
            CHECK(!result);
            std::printf("G09 %s: %s\n", label, result.error().c_str());
        };
        auto changed = original;
        changed[1] = 0x00010700;
        reject(changed, "version");
        changed = original;
        changed[5] = 0;
        reject(changed, "zero-instruction-length");
        changed = original;
        changed.insert(changed.end(), {(2u << 16) | 73, 1});
        reject(changed, "grouped-decoration");
        changed = original;
        changed.insert(changed.end(), {(3u << 16) | 71, 1, 20});
        reject(changed, "aliased-decoration");
        bool descriptor_seen = false, entry_seen = false;
        for (std::size_t i = 5; i < original.size(); i += original[i] >> 16)
        {
            const auto opcode = original[i] & 0xffff, length = original[i] >> 16;
            if (!descriptor_seen && opcode == 71 && length == 4 && original[i + 2] == 33)
            {
                descriptor_seen = true;
                changed = original;
                changed.insert(changed.begin() + i, original.begin() + i, original.begin() + i + 4);
                reject(changed, "duplicate-binding");
                changed = original;
                changed.erase(changed.begin() + i, changed.begin() + i + 4);
                reject(changed, "missing-binding");
                changed = original;
                changed[i + 1] = original[3] - 1;
                reject(changed, "wrong-variable");
            }
            if (!entry_seen && opcode == 15)
            {
                entry_seen = true;
                changed = original;
                changed.insert(changed.begin() + i, original.begin() + i, original.begin() + i + length);
                reject(changed, "multiple-entry-points");
            }
        }
        CHECK(descriptor_seen && entry_seen);
        auto fields = std::vector(schema.resources.begin(), schema.resources.end());
        auto wrong_schema = schema;
        wrong_schema.resources = fields;
        fields[2].array_count = 3;
        CHECK(!relocatePassSpirv(original, wrong_schema, locations, 4));
        fields[2] = schema.resources[2];
        fields[1].role = lux::rdesc::EPassFieldRole::UNIFORM_READ;
        CHECK(!relocatePassSpirv(original, wrong_schema, locations, 4));
        auto wrong_locations = locations;
        wrong_locations[1] = wrong_locations[0];
        CHECK(!relocatePassSpirv(original, schema, wrong_locations, 4));
        const auto vertex = words(binaries / "F3SharedA.vert.spv");
        auto fragment = words(binaries / "F3SharedA.frag.spv");
        auto pair_schema = PassSchema<F3SharedA>::contract();
        std::array modules{PassShaderModule{vertex}, PassShaderModule{fragment}};
        CHECK(validatePassShaders(modules, pair_schema, 3));
        bool interface_changed = false;
        for (std::size_t i = 5; i < fragment.size(); i += fragment[i] >> 16)
        {
            if ((fragment[i] & 0xffff) == 71 && fragment[i + 2] == 30)
            {
                fragment[i + 3] = 7;
                interface_changed = true;
            }
        }
        CHECK(interface_changed);
        auto mismatch = validatePassShaders(modules, pair_schema, 3);
        CHECK(!mismatch && mismatch.error().find("interface mismatch") != std::string::npos);
        std::printf("G09 %s\n", mismatch.error().c_str());
        for (int value = 1; value <= 70; ++value)
        {
            const auto format = static_cast<lux::rdesc::ETextureFormat>(value);
            CHECK(neutralTextureFormat(nativeTextureFormat(format)) == format);
        }
        CHECK(nativeTextureFormat(static_cast<lux::rdesc::ETextureFormat>(999)) == VK_FORMAT_UNDEFINED);
    }
    CHECK(validation.errors.load() == 0);
    std::puts("G08/G09 cold validation complete; rejected inputs never submitted to native GPU creation");
}
