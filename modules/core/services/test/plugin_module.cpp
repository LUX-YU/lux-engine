#include "ServiceProbe.hpp"
#include <array>
#include <cassert>
#include <lux/engine/object/LuxObject.hpp>

namespace
{
    using namespace lux;
    using namespace lux::services;
    class PluginService final : public object::LuxObject, public fixture::Value
    {
    public:
        PluginService(fixture::Trace& trace)
            : LuxObject(), trace_(trace)
        {
            ++trace_.created;
        }
        ~PluginService() override
        {
            assert(trace_.owner == std::this_thread::get_id());
            assert(trace_.unloaded == 0);
            ++trace_.destroyed;
        }
        int read() const noexcept override
        {
            return 73;
        }
        fixture::Trace& trace() noexcept
        {
            return trace_;
        }

    private:
        fixture::Trace& trace_;
    };
    constexpr std::array contracts{
        ServiceContract::forType<PluginService, fixture::Value>(ServiceNameView{"fixture.value"})
    };
    constexpr std::array dependencies{ServiceDependency{
        ServiceNameView{"fixture.trace"},
        1,
        cxx::typeToken<fixture::Trace>(),
        EDependencyKind::BORROWED
    }};
    constexpr auto factory = [](ServiceResolver& resolver,
                                const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<PluginService>>
    {
        auto trace = resolver.require<fixture::Trace>(0);
        if (!trace)
        {
            return cxx::unexpected(std::move(trace.error()));
        }
        return std::make_unique<PluginService>(trace->get());
    };
    constexpr auto descriptor = []
    {
        auto value = ServiceDescriptor::forType<PluginService, factory>(
            ServiceNameView{"fixture.plugin"},
            contracts,
            dependencies
        );
        value.destroy = [](void* pointer) noexcept
        {
            auto* service = static_cast<PluginService*>(pointer);
            auto& trace = service->trace();
            delete service;
            assert(trace.unloaded == 0);
            ++trace.returned; // Executes in the DLL after the virtual destructor returns.
        };
        return value;
    }();
} // namespace
#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" TEST_EXPORT void serviceDefinition(
    lux::object::CodeLease code,
    std::shared_ptr<const lux::services::ServiceEntry>& result
)
{
    result = lux::services::ServiceEntry::bind<descriptor>(std::move(code));
}
