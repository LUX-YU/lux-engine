#include <cassert>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/render/RendererConfig.hpp>

namespace
{
    struct TransientDefinition final
    {
        static constexpr const char* name = "fixture.transient";
        static constexpr const char* message = "Result {0}, flags {1}, count {2}";
        static constexpr auto recovery = lux::render::ERecovery::RETRYABLE;
        static constexpr lux::render::ErrorArgs args{
            lux::render::EErrorArg::VK_RESULT,
            lux::render::EErrorArg::HEX,
            lux::render::EErrorArg::UINT
        };
    };
} // namespace
int main()
{
    assert(lux::render::registerRendererErrors());

    using namespace lux;
    auto& registry = render::renderErrorRegistry();
    // Every existing backend descriptor must be valid in the numeric boundary format.
    for (const auto& [slot, descriptor] : registry.snapshot())
    {
        const auto expected = error::errorId("lux.render.backend." + std::string(descriptor.name));
        assert(error::ErrorRegistry::instance().find(expected));
        const auto value = render::toError(render::makeError(slot, 3, 4, 5));
        assert(value.type == error::errorId("lux.render.backend." + std::string(descriptor.name)));
        const auto* stable = error::ErrorRegistry::instance().find(value.type);
        assert(stable && stable->message == descriptor.message);
    }
    const auto local = render::renderError<TransientDefinition>(render::encodeVkResult(-4), 255, 17);
    const auto stable = render::toError(local);
    assert(stable.type == error::errorId("lux.render.backend.fixture.transient"));
    assert(error::format(stable) == "Result -4, flags 0xff, count 17");
    assert(render::toError(render::RendererFailure{render::ERendererError::DEVICE_FAILURE, local, 99, 5}) == stable);
    registry.forget<TransientDefinition>();
    assert(!registry.find(local.type));
    assert(error::format(stable) == "Result -4, flags 0xff, count 17");
    const auto unknown = render::toError(local);
    assert(unknown.type == error::errorId("lux.render.unavailable_descriptor"));
    assert(unknown.args[0] == local.type.index && unknown.args[1] == local.type.gen);
    assert(render::toError(render::RenderError{}) == error::Error{});

    const auto capacity = render::toError(render::RendererFailure{render::ERendererError::CAPACITY, {}, 92, 8});
    assert(capacity.type == error::errorId("lux.render.runtime.capacity"));
    assert((capacity.args == std::array<std::uint64_t, 3>{92, 1, 8}));
    assert(error::ErrorRegistry::instance().find(capacity.type)->recovery == error::ERecovery::RETRYABLE);
    const auto stopping = render::toError(render::RendererFailure{render::ERendererError::STOPPING});
    assert(stopping.type == error::errorId("lux.render.runtime.stopping"));
    assert(error::ErrorRegistry::instance().find(stopping.type)->recovery == error::ERecovery::PERMANENT);
}
