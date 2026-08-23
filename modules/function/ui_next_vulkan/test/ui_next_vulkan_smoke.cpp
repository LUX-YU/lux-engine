#include <lux/engine/ui_next/UiNext.hpp>
#include <lux/engine/ui_next_vulkan/UiVulkanIntegration.hpp>

#include <cassert>

namespace
{
    class ImagePane final : public lux::object::Object<ImagePane, lux::ui::Pane>
    {
      public:
        ImagePane(
            lux::object::ObjectDispatcher& dispatcher,
            lux::ui::PaneId id,
            ImTextureID texture
        )
            : Object(
                dispatcher,
                std::move(id),
                lux::ui::PaneTypeId{"smoke.image"},
                "Image"
            ),
              texture_(texture)
        {
        }

      protected:
        void draw(lux::ui::PaneDrawContext&) override
        {
            ImGui::Image(texture_, {32.0F, 32.0F});
        }

      private:
        ImTextureID texture_{};
    };
}

int main()
{
    lux::ui::UISession session;
    ImagePane first{
        session.dispatcher(),
        lux::ui::PaneId{"smoke.first"},
        static_cast<ImTextureID>(1)
    };
    ImagePane second{
        session.dispatcher(),
        lux::ui::PaneId{"smoke.second"},
        static_cast<ImTextureID>(2)
    };
    auto first_registration = session.registerPane(first);
    auto second_registration = session.registerPane(second);
    assert(first_registration && second_registration);

    lux::ui::vulkan::DrawDataSnapshot snapshot;
    for (int frame = 0; frame < 2; ++frame)
    {
        const ImVec2 size = frame == 0
            ? ImVec2{320.0F, 180.0F}
            : ImVec2{640.0F, 360.0F};
        session.beginFrame(size, 1.0F / 60.0F);
#ifdef IMGUI_HAS_DOCK
        ImGui::DockSpaceOverViewport();
#endif
        session.drawPanes();
        auto* draw_data = session.endFrame();
        assert(draw_data && draw_data->Valid);
        snapshot.capture(*draw_data);
        const auto summary = lux::ui::vulkan::summarize(
            snapshot.drawData()
        );
        assert(summary.command_lists >= 1);
        assert(summary.vertices > 0);
        assert(summary.indices > 0);

        bool found_first_texture = false;
        bool found_second_texture = false;
        const auto& captured = snapshot.drawData();
        for (int list_index = 0;
             list_index < captured.CmdListsCount;
             ++list_index)
        {
            const auto& commands = captured.CmdLists[list_index]->CmdBuffer;
            for (const auto& command : commands)
            {
                found_first_texture |= command.GetTexID()
                    == static_cast<ImTextureID>(1);
                found_second_texture |= command.GetTexID()
                    == static_cast<ImTextureID>(2);
            }
        }
        if (frame == 0)
        {
            ImGui::SetWindowPos(
                "Image###smoke.first",
                {8.0F, 8.0F}
            );
            ImGui::SetWindowSize(
                "Image###smoke.first",
                {140.0F, 100.0F}
            );
            ImGui::SetWindowPos(
                "Image###smoke.second",
                {164.0F, 8.0F}
            );
            ImGui::SetWindowSize(
                "Image###smoke.second",
                {140.0F, 100.0F}
            );
        }
        else
        {
            assert(found_first_texture);
            assert(found_second_texture);
        }
    }
}
