#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/scene/ConfigurationForm.hpp>
#include <imgui.h>

namespace lux::editor::project
{
    namespace
    {
        class ShortcutFields final : public lux::ui::Element
        {
        public:
            ShortcutFields(lux::ui::Element& parent, lux::ui::ElementId id, ConfigurationValue& value)
                : Element(parent, std::move(id)), value_(value)
            {}
            [[nodiscard]] EditorResult<void> rebuild()
            {
                auto candidate = std::make_unique<detail::ConfigurationForm>(*this, lux::ui::ElementId{"bindings"});
                EditorResult<void> status;
                for (std::size_t i = 0; i < value().overrides.size(); ++i)
                    candidate->add(std::to_string(i), [this, i]() -> auto& { return value().overrides[i]; }, status);
                if (!status)
                    return status;
                fields_ = std::move(candidate);
                return {};
            }
        private:
            ShortcutSettings& value() noexcept { return *static_cast<ShortcutSettings*>(value_.data()); }
            void update() noexcept override
            {
                if (!change_)
                    return;
                const auto change = std::exchange(change_, 0);
                // Controls resolve values through ConfigurationValue; finish them before changing indices.
                finish(*fields_);
                fields_.reset();
                if (change > 0)
                    value().overrides.push_back({"", "", commands::ECommandScope::APPLICATION, 1});
                else
                    value().overrides.pop_back();
                auto rebuilt = rebuild();
                if (!rebuilt)
                    failure_ = rebuilt.error();
            }
            static void finish(lux::object::LuxObject& node) noexcept
            {
                for (auto* child = node.firstChild(); child; child = child->nextSibling())
                    finish(*child);
                static_cast<lux::ui::Element&>(node).finishEdit();
            }
            void arrangeContent() noexcept override
            {
                if (fields_)
                    fields_->arrange({{}, {rect().size.width, std::max(0.f, rect().size.height - 50.f)}});
            }
            void draw() noexcept override
            {
                ImGui::BeginDisabled(value().overrides.size() == 256);
                if (ImGui::Button("Add command override"))
                    change_ = 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(value().overrides.empty());
                if (ImGui::Button("Remove last override"))
                    change_ = -1;
                ImGui::EndDisabled();
                ImGui::TextWrapped("Use the stable command ID and its declared scope/version. Empty binding disables it.");
                if (failure_)
                    ImGui::TextWrapped("%s", failure_->domain.c_str());
                if (fields_)
                    drawChild(*fields_, {0, ImGui::GetCursorPosY() - rect().position.y});
            }
            ConfigurationValue& value_;
            std::unique_ptr<detail::ConfigurationForm> fields_;
            std::optional<EditorFailure> failure_;
            int change_{};
        };
    }
    std::vector<settings::SettingsPage> makeDesktopSettingsPages(settings::SettingsEntry::Apply shortcuts)
    {
        return {
            {makeAppearanceSetting(), detail::configurationEditor<AppearanceSettings>("lux.desktop.appearance").create},
            {makeWindowSetting(), detail::configurationEditor<WindowSettings>("lux.desktop.window").create},
            {makeShortcutSetting(std::move(shortcuts)),
                +[](lux::ui::Element& parent, lux::ui::ElementId id, ConfigurationValue& value) noexcept
                    -> EditorResult<std::unique_ptr<lux::ui::Element>> {
                    auto result = std::make_unique<ShortcutFields>(parent, std::move(id), value);
                    auto created = result->rebuild();
                    if (!created)
                        return cxx::unexpected(created.error());
                    return result;
                }
            }
        };
    }
}
