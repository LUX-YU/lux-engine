#include "../Support.hpp"
#include "../SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/features/MeshFeature.hpp>

int main()
{
    using namespace foundation_test;
    const auto mesh = FeatureProvider<BasicMeshFeature>{}.definition();
    const auto depth = FeatureProvider<DepthOnlyFeature>{}.definition();
    CHECK(mesh.descriptor == &BasicMeshFeature::descriptor && mesh.graph == &BasicMeshFeature::graph && mesh.programs);
    CHECK(depth.descriptor == &DepthOnlyFeature::descriptor && depth.graph == &DepthOnlyFeature::graph && depth.programs);
    Validation validation;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        const native_fixture::Config config;
        auto assembly = take(native_fixture::assemble(config));
        auto code = take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, native_fixture::programs(config), native_fixture::target(config)));
        CHECK(code.passes().size() == 2 && code.passes()[0].pipeline);
    }
    CHECK(validation.errors == 0);
}
