#include <lux/engine/render/features/MeshFeature.hpp>
#include <cstdlib>
#include <cstdio>

using namespace lux::render;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)

template <class T> static T take(RenderResult<T> value) { CHECK(value); return std::move(*value); }

template <class Feature> static RenderResult<FeatureAssembly> assemble(const MeshFeatureConfig& input)
{
    auto config = ConfigValue::make(input);
    if (!config) return lux::cxx::unexpected(config.error());
    const FeatureInstanceView feature{&Feature::descriptor, &Feature::graph, &*config, "main"};
    return assembleFeatures({&feature, 1}, 1024 * 1024);
}

int main()
{
    MeshFeatureConfig input;
    input.target = {64, 64, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    input.mesh.description = {20, 4, 80, 4, 12,
        {{0, 0, lux::rdesc::ETextureFormat::RGB32_SFLOAT}, {1, 12, lux::rdesc::ETextureFormat::RG32_SFLOAT}},
        {{0, 12, 0}}};
    MaterialProgram material;
    material.fragment.words.reserve(100);
    input.material.program = take(ConfigValue::make(std::move(material)));
    input.instances = {2, InstanceDrawGroup{}};
    auto basic = take(assemble<BasicMeshFeature>(input));
    CHECK(basic.definition.passes().size() == 1 && basic.analysis.execution_order.size() == 1);
    CHECK(basic.definition.outputs().size() == 2);
    CHECK(basic.external_bindings.global.size() == 2 && basic.external_bindings.scene.size() == 2);
    CHECK(basic.external_bindings.view.empty() && basic.external_bindings.attachment.empty());
    for (const auto& output : basic.definition.outputs())
        CHECK(std::holds_alternative<GraphImageDesc>(basic.definition.resources()[output.version.resource.index].description));
    auto depth = take(assemble<DepthOnlyFeature>(input));
    CHECK(depth.definition.passes().size() == 2 && depth.analysis.execution_order.size() == 2);
    CHECK(depth.external_bindings.global.size() == 1 && depth.external_bindings.scene.size() == 2);
    input.readback = true;
    auto readback = take(assemble<BasicMeshFeature>(input));
    CHECK(readback.definition.passes().size() == 3 && readback.definition.outputs().size() == 4);
    input.mesh.description.attributes.reserve(64);
    const auto bytes = take(ConfigValueTraits<MeshFeatureConfig>::retainedBytes(input));
    CHECK(bytes >= sizeof(input) + 64 * sizeof(MeshAttribute) + 100 * sizeof(std::uint32_t));
    auto invalid = input;
    invalid.instances.group.reset();
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.instances.group->submesh = 1;
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.material.program = take(ConfigValue::make(3u));
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.textures.push_back({16, 16});
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.mesh.description.attributes[0].format = lux::rdesc::ETextureFormat::RGBA32_SFLOAT;
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.target.width = invalid.target.height = UINT32_MAX;
    CHECK(!assemble<BasicMeshFeature>(invalid));
    invalid = input;
    invalid.instance_storage.clear();
    CHECK(!assemble<BasicMeshFeature>(invalid));
    CHECK(basic.analysis.execution_order.size() == 1 && depth.analysis.execution_order.size() == 2);
    std::puts("Mesh Feature typed contracts, Scene placement, retained budget, explicit readback, invalid candidate isolation");
}
