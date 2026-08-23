#pragma once

#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>

class LUX_OBJECT() InstalledObject final
    : public lux::object::Object<InstalledObject> {
public:
  static const signal_type<int> changed;

  void publish(int value) { notify<changed>(value); }
};
