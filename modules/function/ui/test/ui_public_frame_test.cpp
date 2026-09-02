#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/ui/UI.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string>

static_assert(!std::copy_constructible<lux::ui::Frame>);
static_assert(std::move_constructible<lux::ui::Frame>);
static_assert(std::same_as<decltype(lux::ui::ViewportResult::size), lux::ui::Size>);
static_assert(std::same_as<decltype(lux::ui::UiKey::key), lux::ui::EKey>);

namespace
{
    class PublicPane final : public lux::object::Object<PublicPane, lux::ui::Pane>
    {
    public:
        explicit PublicPane(lux::object::ObjectDispatcherRef dispatcher)
            : Object(
                  std::move(dispatcher),
                  lux::ui::PaneId{"ui.public.frame"},
                  lux::ui::PaneTypeId{"ui.public.frame"},
                  "Public Frame"
              )
        {
        }

        int draw_count{};
        double precise_value{9007199254740991.0};

    private:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext&) override
        {
            ++draw_count;
            assert(frame.theme().metrics.property_label_width > 0.0F);
            frame.text("Lux UI public frame");
            frame.textMuted("backend-neutral");
            bool enabled = true;
            static_cast<void>(frame.checkbox("Enabled", enabled));
            static_cast<void>(frame.editScalar("Double", precise_value));
            {
                auto table = frame.table({lux::ui::WidgetIdView{"ui.public.properties"}, 2U});
                if (table.visible())
                {
                    frame.propertyRow("Value");
                    static_cast<void>(frame.editScalar("##value", precise_value));
                }
            }
            {
                auto disabled = frame.disabled(true);
                static_cast<void>(frame.button("Disabled"));
            }
            {
                auto child = frame.child({lux::ui::WidgetIdView{"ui.public.child"}, {0.0F, 36.0F}, false});
                if (child.visible())
                    frame.text("Child");
            }
            {
                auto row = frame.treeRow({
                    lux::ui::WidgetIdView{"ui.public.tree"},
                    "Tree row",
                    false,
                    true,
                    false
                });
                assert(row.open());
            }
            frame.openPopup(lux::ui::WidgetIdView{"ui.public.popup"});
            auto popup = frame.popup({lux::ui::WidgetIdView{"ui.public.popup"}, false});
            if (popup.visible())
                frame.text("Popup");
        }
    };
} // namespace

int main()
{
    lux::ui::UISession session;
    PublicPane pane{session.dispatcherRef()};
    auto registration = session.registerPane(pane);
    assert(registration);
    auto frame = session.beginFrame({{640.0F, 360.0F}, 1.0F / 60.0F});
    frame.drawPanes();
    frame.finish();
    assert(pane.draw_count == 1);
    assert(pane.precise_value == 9007199254740991.0);
    return 0;
}
