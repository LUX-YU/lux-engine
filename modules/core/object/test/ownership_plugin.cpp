#include <lux/engine/object/LuxObject.hpp>
#include <utility>

using namespace lux::object;
namespace
{
    class PluginObject final : public LuxObject
    {
    public:
        PluginObject(int* trace) : LuxObject(), trace_(trace) {}
        ~PluginObject() override
        {
            ++trace_[0];
        }

    private:
        int* trace_;
    };
    struct PluginDeleter final
    {
        int* trace{};
        explicit PluginDeleter(int* value) noexcept : trace(value) {}
        PluginDeleter(PluginDeleter&& other) noexcept : trace(std::exchange(other.trace, nullptr)) {}
        ~PluginDeleter()
        {
            if (trace)
            {
                ++trace[2];
            }
        }
        void operator()(PluginObject* value) noexcept
        {
            delete value;
            ++trace[1];
        }
    };
} // namespace
#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" TEST_EXPORT void make_object(
    CodeLease code,
    int* trace,
    std::unique_ptr<LuxObject, ObjectDeleter>& result
) noexcept
{
    result = {
        new PluginObject(trace),
        ObjectDeleter::create<PluginObject>(PluginDeleter{trace}, std::move(code))
    };
}

extern "C" TEST_EXPORT ObjectRuntime* object_runtime() noexcept
{
    return &ObjectRuntime::instance();
}
