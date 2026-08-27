#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>

class LUX_OBJECT() MissingCodegenObject final : public lux::object::Object<MissingCodegenObject>
{
public:
    static const signal_type<int> changed;

    void publish(int value) noexcept
    {
        notify<changed>(value);
    }
};

int
main()
{
    MissingCodegenObject object;
    object.publish(1);
}
