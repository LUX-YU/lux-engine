#pragma once

#include "BaseObject.hpp"

#if defined(_WIN32)
#if defined(LUX_OBJECT_CROSS_DLL_DERIVED_LIBRARY)
#define LUX_OBJECT_CROSS_DLL_DERIVED_API __declspec(dllexport)
#else
#define LUX_OBJECT_CROSS_DLL_DERIVED_API __declspec(dllimport)
#endif
#else
#define LUX_OBJECT_CROSS_DLL_DERIVED_API __attribute__((visibility("default")))
#endif

namespace lux::object::cross_dll {
class LUX_OBJECT_CROSS_DLL_DERIVED_API LUX_OBJECT() DerivedObject final
    : public Object<DerivedObject, BaseObject> {
public:
  static const signal_type<int> derivedChanged;
  static const signal_type<int> derivedChangedB;

  void publishDerived(int value) { notify<derivedChanged>(value); }
  void publishDerivedB(int value) { notify<derivedChangedB>(value); }

  LUX_METHOD(connectable = true)
  void receive(const int &value) noexcept { last_value = value; }

  LUX_METHOD(connectable = true)
  void receiveB(const int &value) noexcept { last_value_b = value; }

  int last_value{0};
  int last_value_b{0};
};
} // namespace lux::object::cross_dll
