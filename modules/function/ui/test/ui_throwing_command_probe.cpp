#include <lux/engine/ui/UI.hpp>

class Receiver final : public lux::object::Object<Receiver>
{
public:
    void invoke()
    {
    }
};

int
main()
{
    lux::ui::CommandRouter router;
    Receiver receiver;
    const auto command = router.defineCommand({lux::ui::UiCommandId{"probe"}, "Probe"});
    if (!command)
        return 1;
    const auto binding = router.bindGlobal<&Receiver::invoke>(*command, receiver);
    return binding ? 0 : 2;
}
