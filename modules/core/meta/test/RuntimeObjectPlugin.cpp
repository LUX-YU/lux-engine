#include "RuntimeObjectPlugin.hpp"

#include <memory>
#include <string>

namespace
{
    RuntimeObjectProbe* probe{};

    struct PluginValue
    {
        std::string text = std::string(256, 'p');

        PluginValue()
        {
            ++probe->constructed;
        }

        PluginValue(const PluginValue& other) : text(other.text)
        {
            ++probe->copied;
        }

        ~PluginValue()
        {
            ++probe->destroyed;
        }
    };

    void registerValue(lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list&)
    {
        auto cls = std::make_unique<lux::meta::RefClass>();
        cls->name = "PluginValue";
        cls->full_name = "test.runtime.PluginValue";
        cls->hash = lux::cxx::type_hash<PluginValue>();
        cls->type = lux::meta::ref_type_of_v<PluginValue>;
        cls->type.ptr = cls.get();
        cls->construct = [](void* memory) { new (memory) PluginValue(); };
        cls->destruct = [](void* memory)
        {
            static_cast<PluginValue*>(memory)->~PluginValue();
            ++probe->destruct_tail;
        };
        lux::meta::ref_class_func_gen<PluginValue>(*cls);
        registry.registerClass(std::move(cls));
    }
} // namespace

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT RuntimeObjectRegistration runtimeObjectRegistration(RuntimeObjectProbe* value) noexcept
{
    probe = value;
    return registerValue;
}
