#include "app/ImportDialog.hpp"

#include <imgui.h>

namespace lux::editor
{
    void ImportDialog::paint()
    {
        if (open_)
        {
            ImGui::OpenPopup("Import Options");
            open_ = false;
        }

        const auto centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Import Options", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        auto& o = options_;

        ImGui::TextUnformatted(source_.filename().string().c_str());
        ImGui::Separator();

        // Up-axis presets — the common one is glTF Z-up → this engine's Y-up,
        // i.e. -90° about X. "Custom" lets the user dial arbitrary Euler angles.
        static const char* kPresets[] = {
            "None (Y-up)", "Z-up -> Y-up (-90 X)", "+90 about X",
            "180 about X", "Custom"
        };
        int preset = 4; // Custom unless it matches a known preset
        if (o.pre_rotate_y_deg == 0.f && o.pre_rotate_z_deg == 0.f)
        {
            if (o.pre_rotate_x_deg == 0.f)        preset = 0;
            else if (o.pre_rotate_x_deg == -90.f) preset = 1;
            else if (o.pre_rotate_x_deg == 90.f)  preset = 2;
            else if (o.pre_rotate_x_deg == 180.f) preset = 3;
        }
        if (ImGui::Combo("Up-axis fix", &preset, kPresets, IM_ARRAYSIZE(kPresets)))
        {
            o.pre_rotate_y_deg = 0.f;
            o.pre_rotate_z_deg = 0.f;
            switch (preset)
            {
            case 0: o.pre_rotate_x_deg = 0.f;   break;
            case 1: o.pre_rotate_x_deg = -90.f; break;
            case 2: o.pre_rotate_x_deg = 90.f;  break;
            case 3: o.pre_rotate_x_deg = 180.f; break;
            default: break; // Custom — keep current values
            }
        }

        float xyz[3] = { o.pre_rotate_x_deg, o.pre_rotate_y_deg, o.pre_rotate_z_deg };
        if (ImGui::DragFloat3("Rotate XYZ (deg)", xyz, 1.0f, -180.f, 180.f))
        {
            o.pre_rotate_x_deg = xyz[0];
            o.pre_rotate_y_deg = xyz[1];
            o.pre_rotate_z_deg = xyz[2];
        }
        ImGui::DragFloat("Uniform scale", &o.uniform_scale, 0.01f, 0.0001f, 1000.f,
                         "%.4f");
        ImGui::Checkbox("Import animations", &o.import_animations);

        ImGui::Spacing();
        const float bw = 120.f;
        if (ImGui::Button("Import", ImVec2(bw, 0.f)))
        {
            const auto src  = source_;
            const auto opts = options_;
            ImGui::CloseCurrentPopup();
            // The actual import is deferred out of the ImGui frame by the editor
            // (same rule as the menu / drop paths).
            if (on_confirm_)
                on_confirm_(src, opts);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw, 0.f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

} // namespace lux::editor
