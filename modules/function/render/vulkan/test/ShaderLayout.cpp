#include "Complex.pass.hpp"
#include "Stages.pass.hpp"
#include "Support.hpp"
#include <fstream>
#include <lux/engine/render/vulkan/shader/Layout.hpp>
#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>

using namespace foundation_test;

static std::vector<std::uint32_t> readSpirv(const char* path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    CHECK(stream && stream.tellg() > 0 && stream.tellg() % 4 == 0);
    const auto bytes = static_cast<std::size_t>(stream.tellg());
    std::vector<std::uint32_t> words(bytes / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(words.data()), bytes);
    CHECK(stream);
    return words;
}

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    Validation validation;
    {
        auto instance_owner = instance(validation);
        auto device = take(VulkanDevice::create(instance_owner));
        const auto caps = queryLayoutCaps(device);
        const auto schema = lux::render::PassSchema<Complex>::contract();
        const std::array schemas{schema};
        auto feature = take(declareOwnerShape("feature.full", 1, lux::rdesc::EFieldOwner::FEATURE, schemas));
        auto local = take(declareOwnerShape("pass.local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas));
        auto other = take(declareOwnerShape("feature.other", 1, lux::rdesc::EFieldOwner::FEATURE, schemas));
        std::array owners{feature, local, other};
        OwnerAssignment assignment{{}, feature.identity, local.identity};
        auto layout = take(compileLayout(schema, assignment, owners, caps));
        CHECK(layout.identity().sets.size() == 2);
        CHECK(layout.identity().sets[0].bindings.size() == 2);
        CHECK(layout.identity().owners.size() == 3);
        CHECK(layout.identity().fields.size() == 3);
        std::reverse(owners.begin(), owners.end());
        CHECK(layout.matches(take(compileLayout(schema, assignment, owners, caps))));
        std::printf("%s", layout.diagnostics().c_str());
        auto lowered = caps;
        lowered.limits.maxBoundDescriptorSets = 1;
        CHECK(!compileLayout(schema, assignment, owners, lowered));
        lowered = caps;
        lowered.limits.maxPushConstantsSize = 4;
        CHECK(!compileLayout(schema, assignment, owners, lowered));
        lowered = caps;
        lowered.limits.maxPerStageDescriptorUniformBuffers = 1;
        CHECK(!compileLayout(schema, assignment, owners, lowered));
        auto invalid = owners;
        invalid[0].fields[0].count += 1;
        // other owner is not used by this shader: its full shape still participates in budget.
        invalid[1].fields[0].flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        CHECK(!compileLayout(schema, assignment, invalid, caps));
        invalid = owners;
        invalid[0].fields.push_back(invalid[0].fields[0]);
        CHECK(!compileLayout(schema, assignment, invalid, caps));
        invalid = owners;
        invalid[0].fields.clear();
        CHECK(!compileLayout(schema, assignment, invalid, caps));
        auto missing_owner = assignment;
        missing_owner.feature = other.identity;
        CHECK(compileLayout(schema, missing_owner, owners, caps));
        missing_owner.feature = ShaderOwnerId{123};
        CHECK(!compileLayout(schema, missing_owner, owners, caps));

        std::vector<lux::toolchain::PassDescriptorLocation> locations;
        for (const auto& field : layout.identity().fields)
        {
            locations.push_back({field.field_index, field.set, field.binding});
        }
        const auto binary = readSpirv(argv[1]);
        auto relocated = lux::toolchain::relocatePassSpirv(binary, schema, locations, 4);
        if (!relocated)
        {
            std::fprintf(stderr, "%s\n", relocated.error().c_str());
        }
        CHECK(relocated);
        CHECK(relocated->words != binary);
        CHECK(lux::toolchain::validatePassSpirv(relocated->words, schema, locations));
        CHECK(!lux::toolchain::relocatePassSpirv(binary, schema, locations, 1));
        auto corrupt = binary;
        corrupt[5] &= 0xffff;
        CHECK(!lux::toolchain::relocatePassSpirv(corrupt, schema, locations, 4));
        for (std::size_t p = 5; p < binary.size(); p += binary[p] >> 16)
        {
            if ((binary[p] & 0xffff) == 71 && binary[p + 2] == 33)
            {
                corrupt = binary;
                corrupt.insert(corrupt.end(), binary.begin() + p, binary.begin() + p + 4);
                auto duplicate = lux::toolchain::relocatePassSpirv(corrupt, schema, locations, 4);
                CHECK(!duplicate && duplicate.error().find("Duplicate") != std::string::npos);
                corrupt = binary;
                corrupt.erase(corrupt.begin() + p, corrupt.begin() + p + 4);
                CHECK(!lux::toolchain::relocatePassSpirv(corrupt, schema, locations, 4));
                break;
            }
        }
        const auto stage_schema = lux::render::PassSchema<Stages>::contract();
        const std::array stage_schemas{stage_schema};
        auto stage_owner =
            take(declareOwnerShape("graphics.local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, stage_schemas));
        const std::array stage_owners{stage_owner};
        auto stage_layout = take(compileLayout(stage_schema, {{}, {}, stage_owner.identity}, stage_owners, caps));
        locations.clear();
        for (const auto& field : stage_layout.identity().fields)
        {
            locations.push_back({field.field_index, field.set, field.binding});
        }
        CHECK(lux::toolchain::relocatePassSpirv(readSpirv(argv[2]), stage_schema, locations, 1));
        CHECK(lux::toolchain::relocatePassSpirv(readSpirv(argv[3]), stage_schema, locations, 2));
        std::printf(
            "CAPS sets=%u PC=%u UBO-align=%llu SSBO-align=%llu vendor=%u device=%u driver=%u api=%u\n",
            caps.limits.maxBoundDescriptorSets,
            caps.limits.maxPushConstantsSize,
            static_cast<unsigned long long>(caps.limits.minUniformBufferOffsetAlignment),
            static_cast<unsigned long long>(caps.limits.minStorageBufferOffsetAlignment),
            caps.vendor_id,
            caps.device_id,
            caps.driver_version,
            caps.api_version
        );
    }
    CHECK(validation.errors.load() == 0);
    std::puts("F3 Layout/relocation CPU contracts PASS; actual device caps queried");
}
