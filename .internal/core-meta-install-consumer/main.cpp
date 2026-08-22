#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/meta/Meta.hpp>

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    const bool initialized = lux::meta::ReflectionRegistry::initialized();
    lux::serialize::NameTable names;
    const auto index = names.intern("installed.consumer");
    lux::meta::ReflectionRegistry::destroyRegistry();
    return initialized && index != 0u ? 0 : 1;
}
