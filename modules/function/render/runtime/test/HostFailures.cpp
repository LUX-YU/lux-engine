#include "NativeGraphSupport.hpp"
#include <array>
#include <atomic>
#include <lux/engine/render/runtime/NativeAssembly.hpp>

namespace runtime_host_test
{
    extern unsigned fail_operation;
    extern std::array<unsigned, 3> created;
    extern unsigned closed;
    extern std::atomic<unsigned> workers;
} // namespace runtime_host_test

using namespace native_graph_test;

int main()
{
    Validation validation;
    RuntimeNativeOptions native;
    native.instance = {{}, true, diagnostic, &validation};
    for (unsigned operation = 1; operation <= 3; ++operation)
    {
        // Retry and partial-construction destruction are separate lifetimes.
        for (bool retry : {false, true})
        {
            auto builder = checked(RenderRuntimeBuilder::create(native));
            auto* const backend = &builder.backend();
            const auto device = backend->device().native();
            runtime_host_test::fail_operation = operation;
            const auto failure = std::move(builder).finish();
            CHECK(!failure);
            CHECK(failure.error().type == lux::error::errorId("lux.render.runtime.host_failure"));
            CHECK(runtime_host_test::fail_operation == 0);
            CHECK(&builder.backend() == backend);
            CHECK(builder.backend().device().native() == device);
            CHECK(runtime_host_test::workers == 0);
            CHECK(
                runtime_host_test::closed ==
                runtime_host_test::created[0] + runtime_host_test::created[1] + runtime_host_test::created[2]
            );
            auto moved = std::move(builder);
            CHECK(&moved.backend() == backend);
            CHECK(moved.backend().addScene());
            if (retry)
            {
                auto runtime = checked(std::move(moved).finish());
                runtime->requestStop();
                CHECK(checked(runtime->waitStopped(5000)));
                CHECK(runtime->state() == ERuntimeState::STOPPED);
                runtime.reset();
            }
            CHECK(runtime_host_test::workers == 0);
            CHECK(
                runtime_host_test::closed ==
                runtime_host_test::created[0] + runtime_host_test::created[1] + runtime_host_test::created[2]
            );
        }
    }
    CHECK(validation.errors == 0);
    std::printf(
        "host-failure branches=event,timer,thread retry/move/destruction=PASS handles=%u workers=0\n",
        runtime_host_test::closed
    );
}
