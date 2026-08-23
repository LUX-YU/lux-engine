#include <lux/engine/object/ObjectModel.hpp>

class InstalledObject final : public lux::object::Object<InstalledObject>
{
  public:
    inline static constexpr signal_type<int> changed{"changed"};
};

int main()
{
    InstalledObject object;
    return object.objectType() == lux::cxx::typeToken<InstalledObject>()
        ? 0
        : 1;
}
