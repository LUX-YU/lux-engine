#include "../Support.hpp"
#include "../SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>

int main()
{
    using namespace foundation_test;
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
