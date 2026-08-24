#include <lux/engine/ui/UI.hpp>

#include <thread>
#include <utility>

namespace
{
    class ProbePane final : public lux::object::Object<ProbePane, lux::ui::Pane>
    {
      public:
        ProbePane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id)
            : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"probe.pane"},
                     "Probe")
        {
        }

      private:
        void draw(lux::ui::PaneDrawContext &) override
        {
        }
    };
} // namespace

int main()
{
    lux::ui::UISession session;
    ProbePane pane{session.dispatcherRef(), lux::ui::PaneId{"probe"}};
    auto registered = session.registerPane(pane);
    if (!registered)
        return 0;

    std::thread foreign(
        [registration = std::move(*registered)]() mutable { registration.reset(); });
    foreign.join();
    return 0;
}
