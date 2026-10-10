#include "Complex.pass.hpp"
#include "Support.hpp"
#include <fstream>
#include <lux/engine/toolchain/shader/ShaderAssets.hpp>

using namespace foundation_test;
using namespace lux::toolchain;

static std::vector<std::uint32_t> readWords(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input && input.tellg() > 0 && input.tellg() % 4 == 0);
    const auto bytes = static_cast<std::size_t>(input.tellg());
    std::vector<std::uint32_t> words(bytes / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), bytes);
    CHECK(input);
    return words;
}

static std::string readText(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    const auto schema = lux::render::PassSchema<Complex>::contract();
    ShaderBuildInputs inputs{
        "fixtures/Complex",
        "default",
        "glslc VulkanSDK 1.4.304.0",
        "vulkan1.3",
        {{"fixtures/Complex.comp.lglsl", readText(argv[2])}},
        {}
    };
    const std::vector stages{ShaderStageBinary{4, readWords(argv[1])}};
    auto candidate = makeCompiledShaderVariant(inputs, stages, schema);
    if (!candidate)
    {
        std::fprintf(stderr, "%s\n", candidate.error().c_str());
    }
    CHECK(candidate);
    const auto identity = candidate->identity();
    ShaderVariantCatalog catalog(1);
    auto inserted = catalog.publish(std::move(*candidate));
    CHECK(inserted && !*inserted && catalog.size() == 1);
    CHECK(catalog.find(identity) != nullptr);
    auto changed = identity;
    changed.inputs.compiler_identity += " changed";
    CHECK(catalog.find(changed) == nullptr);
    changed = identity;
    changed.inputs.sources[0].content += "\n// changed source\n";
    CHECK(catalog.find(changed) == nullptr);
    changed = identity;
    changed.schema_identity.push_back(0);
    CHECK(catalog.find(changed) == nullptr);
    changed = identity;
    changed.stages[0].words[3] += 1;
    CHECK(catalog.find(changed) == nullptr);
    auto bad_stages = stages;
    bad_stages[0].words[0] = 0;
    CHECK(!makeCompiledShaderVariant(inputs, bad_stages, schema));
    CHECK(catalog.find(identity) != nullptr); // Candidate failure leaves last-good directory entry intact.
    auto bad_inputs = inputs;
    bad_inputs.sources.push_back(bad_inputs.sources[0]);
    CHECK(!makeCompiledShaderVariant(bad_inputs, stages, schema));
    bad_inputs = inputs;
    bad_inputs.defines = {{"MODE", "1"}, {"MODE", "2"}};
    CHECK(!makeCompiledShaderVariant(bad_inputs, stages, schema));
    bad_inputs = inputs;
    bad_inputs.target_environment = "unknown";
    CHECK(!makeCompiledShaderVariant(bad_inputs, stages, schema));
    auto second_inputs = inputs;
    second_inputs.variant_name = "another";
    auto second = makeCompiledShaderVariant(second_inputs, stages, schema);
    CHECK(second);
    CHECK(!catalog.publish(std::move(*second)));
    CHECK(catalog.find(identity) != nullptr);
    inputs.sources[0].content += "\n// new asset revision\n";
    auto replacement = makeCompiledShaderVariant(inputs, stages, schema);
    CHECK(replacement);
    const auto new_identity = replacement->identity();
    auto old = catalog.publish(std::move(*replacement));
    CHECK(old && *old && (*old)->identity() == identity);
    CHECK(catalog.find(identity) == nullptr && catalog.find(new_identity) != nullptr);
    // Old compiled bytes remain owned by the returned value, without shared-owner churn.
    CHECK((*old)->identity().stages == stages);
    auto removed = catalog.remove(inputs.asset_name, inputs.variant_name);
    CHECK(removed && *removed && catalog.size() == 0);
    CHECK(!catalog.remove(inputs.asset_name, inputs.variant_name));
    CHECK(catalog.publish(std::move(*makeCompiledShaderVariant(second_inputs, stages, schema))));
    ShaderVariantCatalog empty(0);
    CHECK(!empty.publish(std::move(*makeCompiledShaderVariant(inputs, stages, schema))));
    std::puts("F3 Shader Asset/Variant exact identity, bounded capacity, replacement and last-good CPU PASS");
}
