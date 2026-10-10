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

    struct PluginValue final
    {
        int* trace;

        ~PluginValue()
        {
            ++trace[0];
        }
    };

    struct ValueDeleter final
    {
        int* trace;

        explicit ValueDeleter(int* value) noexcept : trace(value)
        {
        }

        ValueDeleter(ValueDeleter&& other) noexcept : trace(std::exchange(other.trace, nullptr))
        {
        }

        ~ValueDeleter()
        {
            if (trace)
            {
                ++trace[2];
            }
        }

        void operator()(PluginValue* value) noexcept
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
    std::shared_ptr<LuxObject>& result
) noexcept
{
    auto candidate = std::unique_ptr<PluginObject, PluginDeleter>(new PluginObject(trace), PluginDeleter{trace});
    auto shared = shareOnRuntime(std::move(candidate), code);
    if (!shared)
        std::terminate();
    result = std::move(*shared);
}

extern "C" TEST_EXPORT ObjectRuntime* object_runtime() noexcept
{
    return &ObjectRuntime::instance();
}

extern "C" TEST_EXPORT void make_pinned_value(
    CodeLease code,
    int* trace,
    std::shared_ptr<const void>& result
) noexcept
{
    auto value = std::unique_ptr<PluginValue, ValueDeleter>(new PluginValue{trace}, ValueDeleter{trace});
    result = pinCodeOwner(std::move(code), std::move(value));
}
