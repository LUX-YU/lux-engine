#include <consumer.inspector.generated.hpp>
#include <consumer/Gui.hpp>

namespace consumer
{
    namespace
    {
        std::size_t draws{};

        void draw(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object, lux::ui::Frame &frame,
                  lux::editor::gui::InspectorInteraction &interaction)
        {
            ++draws;
            lux::editor::gui::generated::consumerBindings().front().draw(document, object, frame, interaction);
        }
    } // namespace

    lux::editor::gui::ComponentBinding binding()
    {
        auto result = lux::editor::gui::generated::consumerBindings().front();
        result.draw = draw;
        return result;
    }

    std::size_t drawCount() noexcept
    {
        return draws;
    }
} // namespace consumer
