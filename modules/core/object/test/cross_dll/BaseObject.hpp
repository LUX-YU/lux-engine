#pragma once

#include <lux/engine/object/ObjectModel.hpp>

#if defined(_WIN32)
#if defined(LUX_OBJECT_CROSS_DLL_BASE_LIBRARY)
#define LUX_OBJECT_CROSS_DLL_BASE_API __declspec(dllexport)
#else
#define LUX_OBJECT_CROSS_DLL_BASE_API __declspec(dllimport)
#endif
#else
#define LUX_OBJECT_CROSS_DLL_BASE_API __attribute__((visibility("default")))
#endif

namespace lux::object::cross_dll
{
    class LUX_OBJECT_CROSS_DLL_BASE_API LUX_OBJECT() BaseObject
        : public Object<BaseObject>
    {
    public:
        static const signal_type<int> baseChanged;

        void publishBase(int value) { notify<baseChanged>(value); }
    };

    // Deliberately not exported. Its generated descriptor must remain usable
    // inside the declaring module without creating a public DLL symbol.
    class LUX_OBJECT() InternalObject final : public Object<InternalObject>
    {
    public:
        static const signal_type<> internalChanged;
    };
} // namespace lux::object::cross_dll
