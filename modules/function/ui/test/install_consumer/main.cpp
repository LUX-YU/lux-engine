#include <lux/engine/ui/UI.hpp>

int
main()
{
    lux::ui::UISession session;
    auto frame = session.beginFrame({{320.0F, 200.0F}, 1.0F / 60.0F});
    frame.drawPanes();
    const auto& theme = frame.theme();
    frame.finish();
    return theme.metrics.row_height > 0.0F ? 0 : 1;
}
