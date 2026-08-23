#pragma once

#include <lux/engine/object/ObjectModel.hpp>

class LUX_OBJECT() InstalledObject final : public lux::object::Object<InstalledObject>
{
public:
    static const signal_type<int> changed;

    void publish(int value) { notify<changed>(value); }
};
